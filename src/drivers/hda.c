// src/drivers/hda.c
// Simplified Intel HDA driver adapted from https://github.com/inclementine/intelHDA
// for KiwiOS. The port keeps the public interface used by the rest of the
// kernel while reusing the smaller ECE391-oriented implementation as a base.

#include "drivers/hda.h"
#include "drivers/pci.h"
#include "drivers/timer.h"
#include "memory/vmm.h"
#include "memory/pmm.h"
#include "arch/x86/io.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef PAGE_PRESENT
#define PAGE_PRESENT (1u << 0)
#endif
#ifndef PAGE_WRITE
#define PAGE_WRITE (1u << 1)
#endif
#ifndef PAGE_PWT
#define PAGE_PWT (1u << 3)
#endif
#ifndef PAGE_PCD
#define PAGE_PCD (1u << 4)
#endif

// MMIO helper to map HDA BAR uncached (mirrors the AHCI helper used elsewhere).
static void* hda_map_mmio_uncached(uint64_t phys, size_t size) {
    page_table_t* kpt = vmm_get_kernel_page_table();
    if (!kpt) return NULL;

    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base_va = (uint64_t)phys_to_virt(phys);

    for (size_t i = 0; i < pages; ++i) {
        uint64_t pa = phys + i * PAGE_SIZE;
        uint64_t va = base_va + i * PAGE_SIZE;
        vmm_map_page(kpt, va, pa, PAGE_PRESENT | PAGE_WRITE | PAGE_PWT | PAGE_PCD);
    }

    return (void*)base_va;
}

// PCI class codes for High Definition Audio
#define HDA_PCI_CLASS    0x04
#define HDA_PCI_SUBCLASS 0x03

// Register offsets (subset used by the simplified port)
#define HDA_REG_GCAP      0x00
#define HDA_REG_VMIN      0x02
#define HDA_REG_VMAJ      0x03
#define HDA_REG_GCTL      0x08
#define HDA_REG_STATESTS  0x0E
#define HDA_REG_INTCTL    0x20
#define HDA_REG_CORBLBASE 0x40
#define HDA_REG_CORBUBASE 0x44
#define HDA_REG_CORBWP    0x48
#define HDA_REG_CORBRP    0x4A
#define HDA_REG_CORBCTL   0x4C
#define HDA_REG_CORBSTS   0x4D
#define HDA_REG_CORBSIZE  0x4E
#define HDA_REG_RIRBLBASE 0x50
#define HDA_REG_RIRBUBASE 0x54
#define HDA_REG_RIRBWP    0x58
#define HDA_REG_RINTCNT   0x5A
#define HDA_REG_RIRBCTL   0x5C
#define HDA_REG_RIRBSTS   0x5D
#define HDA_REG_RIRBSIZE  0x5E
#define HDA_REG_IC        0x60
#define HDA_REG_IR        0x64
#define HDA_REG_ICS       0x68

// Bit helpers
#define HDA_GCTL_CRST     (1u << 0)
#define HDA_GCTL_UNSOL    (1u << 8)

#define HDA_CORBCTL_RUN   (1u << 1)
#define HDA_CORBRP_RST    (1u << 15)
#define HDA_CORBSTS_INT   (1u << 0)

#define HDA_RIRBCTL_RUN   (1u << 0)
#define HDA_RIRBCTL_IRQ   (1u << 1)
#define HDA_RIRBCTL_OVF   (1u << 2)
#define HDA_RIRBWP_RST    (1u << 15)
#define HDA_RIRBSTS_INT   (1u << 0)
#define HDA_RIRBSTS_OVF   (1u << 2)

#define HDA_INTCTL_GIE    (1u << 31)

// Immediate command bits
#define HDA_ICS_ICB       (1u << 0)
#define HDA_ICS_IRV       (1u << 1)

// Verbs/params (subset)
#define HDA_VERB_GET_PARAMETER 0xF00
#define HDA_VERB_SET_POWER_STATE 0x705
#define HDA_VERB_GET_POWER_STATE 0xF05

#define HDA_PARAM_NODE_COUNT 0x04

// Verb helpers
static inline uint32_t hda_make_verb(uint8_t codec, uint8_t node, uint16_t verb, uint16_t payload) {
    return ((uint32_t)codec << 28) | ((uint32_t)node << 20) | ((verb & 0xFFFu) << 8) | (payload & 0xFFu);
}

// Driver state
typedef struct {
    bool        present;
    bool        reset_ok;
    bool        corb_rirb_ready;

    pci_device_t pci;
    volatile uint8_t* mmio;
    uint32_t    mmio_size;

    uint16_t    gcap;
    uint8_t     vmin;
    uint8_t     vmaj;

    uint16_t    codec_mask;
    uint8_t     primary_codec;

    volatile uint32_t* corb;
    volatile uint64_t* rirb;
    uint32_t    corb_entries;
    uint32_t    rirb_entries;
} hda_state_t;

static hda_state_t g_hda = {0};

static uint64_t hda_deadline_ms(uint32_t ms) {
    uint32_t freq = timer_get_frequency();
    uint64_t ticks = (freq == 0) ? ms : ((uint64_t)ms * freq + 999) / 1000;
    return timer_get_ticks() + ticks;
}

static inline uint8_t hda_mmio_read8(size_t offset) {
    return *(volatile uint8_t*)(g_hda.mmio + offset);
}

static inline uint16_t hda_mmio_read16(size_t offset) {
    return *(volatile uint16_t*)(g_hda.mmio + offset);
}

static inline uint32_t hda_mmio_read32(size_t offset) {
    return *(volatile uint32_t*)(g_hda.mmio + offset);
}

static inline void hda_mmio_write8(size_t offset, uint8_t value) {
    *(volatile uint8_t*)(g_hda.mmio + offset) = value;
}

static inline void hda_mmio_write16(size_t offset, uint16_t value) {
    *(volatile uint16_t*)(g_hda.mmio + offset) = value;
}

static inline void hda_mmio_write32(size_t offset, uint32_t value) {
    *(volatile uint32_t*)(g_hda.mmio + offset) = value;
}

// Reset controller per Intel spec.
static bool hda_reset_controller(void) {
    uint32_t gctl = hda_mmio_read32(HDA_REG_GCTL);
    gctl &= ~HDA_GCTL_CRST;
    hda_mmio_write32(HDA_REG_GCTL, gctl);

    uint64_t deadline = hda_deadline_ms(100);
    while ((hda_mmio_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) && timer_get_ticks() < deadline) {}

    gctl |= HDA_GCTL_CRST;
    hda_mmio_write32(HDA_REG_GCTL, gctl);

    deadline = hda_deadline_ms(100);
    while ((hda_mmio_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) == 0 && timer_get_ticks() < deadline) {}

    return (hda_mmio_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) != 0;
}

// Initialize CORB/RIRB rings using the layout from the upstream intelHDA driver.
static bool hda_init_corb_rirb(void) {
    uint8_t corb_size_caps = hda_mmio_read8(HDA_REG_CORBSIZE);
    uint8_t rirb_size_caps = hda_mmio_read8(HDA_REG_RIRBSIZE);

    uint8_t corb_code = (corb_size_caps & 0x40) ? 0x02 : (corb_size_caps & 0x20) ? 0x01 : 0x00;
    uint8_t rirb_code = (rirb_size_caps & 0x40) ? 0x02 : (rirb_size_caps & 0x20) ? 0x01 : 0x00;

    g_hda.corb_entries = (corb_code == 0x02) ? 256 : (corb_code == 0x01) ? 16 : 2;
    g_hda.rirb_entries = (rirb_code == 0x02) ? 256 : (rirb_code == 0x01) ? 16 : 2;

    size_t corb_bytes = g_hda.corb_entries * sizeof(uint32_t);
    size_t rirb_bytes = g_hda.rirb_entries * sizeof(uint64_t);

    size_t corb_pages = (corb_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t rirb_pages = (rirb_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    void* corb_phys = pmm_alloc_pages(corb_pages);
    void* rirb_phys = pmm_alloc_pages(rirb_pages);
    if (!corb_phys || !rirb_phys) return false;

    g_hda.corb = (volatile uint32_t*)phys_to_virt((uint64_t)corb_phys);
    g_hda.rirb = (volatile uint64_t*)phys_to_virt((uint64_t)rirb_phys);
    memset((void*)g_hda.corb, 0, corb_bytes);
    memset((void*)g_hda.rirb, 0, rirb_bytes);

    uint64_t corb_addr = (uint64_t)corb_phys;
    uint64_t rirb_addr = (uint64_t)rirb_phys;

    hda_mmio_write32(HDA_REG_CORBLBASE, (uint32_t)(corb_addr & 0xFFFFFFFFu));
    hda_mmio_write32(HDA_REG_CORBUBASE, (uint32_t)(corb_addr >> 32));
    hda_mmio_write32(HDA_REG_RIRBLBASE, (uint32_t)(rirb_addr & 0xFFFFFFFFu));
    hda_mmio_write32(HDA_REG_RIRBUBASE, (uint32_t)(rirb_addr >> 32));

    hda_mmio_write8(HDA_REG_CORBSIZE, corb_code);
    hda_mmio_write8(HDA_REG_RIRBSIZE, rirb_code);

    hda_mmio_write16(HDA_REG_CORBRP, HDA_CORBRP_RST);
    hda_mmio_write16(HDA_REG_RIRBWP, HDA_RIRBWP_RST);

    // Start DMA engines
    hda_mmio_write8(HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    hda_mmio_write8(HDA_REG_RIRBCTL, HDA_RIRBCTL_RUN);

    return true;
}

// Immediate command helper (avoids CORB dependency for simple verbs).
static bool hda_send_verb_immediate(uint8_t codec, uint8_t node, uint16_t verb, uint16_t payload, uint32_t* out_resp) {
    uint64_t idle_deadline = hda_deadline_ms(100);
    while ((hda_mmio_read16(HDA_REG_ICS) & HDA_ICS_ICB) && timer_get_ticks() < idle_deadline) {}

    if (hda_mmio_read16(HDA_REG_ICS) & HDA_ICS_ICB) return false;

    uint32_t cmd = hda_make_verb(codec, node, verb, payload);
    hda_mmio_write32(HDA_REG_IC, cmd);

    uint64_t done_deadline = hda_deadline_ms(100);
    while ((hda_mmio_read16(HDA_REG_ICS) & HDA_ICS_IRV) == 0 && timer_get_ticks() < done_deadline) {}

    uint16_t ics = hda_mmio_read16(HDA_REG_ICS);
    if ((ics & HDA_ICS_IRV) == 0) return false;

    if (out_resp) *out_resp = hda_mmio_read32(HDA_REG_IR);
    hda_mmio_write16(HDA_REG_ICS, ics);
    return true;
}

bool hda_init(void) {
    pci_device_t dev;
    if (!pci_find_class_subclass(HDA_PCI_CLASS, HDA_PCI_SUBCLASS, &dev)) {
        return false;
    }

    g_hda.pci = dev;
    g_hda.present = true;

    bool is_io = false;
    uint8_t bar_size_bits = 0;
    uint32_t bar0 = pci_read_bar(dev.bus, dev.slot, dev.func, 0, &is_io, &bar_size_bits);
    if (is_io || bar0 == 0) {
        return false;
    }

    uint64_t phys_base = bar0 & ~0xFu;
    if ((bar0 & 0x6) == 0x4) {
        // 64-bit BAR
        uint32_t bar1 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x14);
        phys_base |= ((uint64_t)bar1 << 32);
    }

    g_hda.mmio_size = (bar_size_bits > 0) ? (1u << bar_size_bits) : 0x4000;
    g_hda.mmio = (volatile uint8_t*)hda_map_mmio_uncached(phys_base, g_hda.mmio_size);
    if (!g_hda.mmio) return false;

    pci_enable_mmio_and_bus_mastering(dev.bus, dev.slot, dev.func);

    g_hda.gcap = hda_mmio_read16(HDA_REG_GCAP);
    g_hda.vmin = hda_mmio_read8(HDA_REG_VMIN);
    g_hda.vmaj = hda_mmio_read8(HDA_REG_VMAJ);

    g_hda.reset_ok = hda_reset_controller();

    uint64_t codec_deadline = hda_deadline_ms(500);
    while (g_hda.codec_mask == 0 && timer_get_ticks() < codec_deadline) {
        g_hda.codec_mask = hda_mmio_read16(HDA_REG_STATESTS) & 0x7FFFu;
    }
    if (g_hda.codec_mask) {
        for (uint8_t i = 0; i < 15; ++i) {
            if (g_hda.codec_mask & (1u << i)) { g_hda.primary_codec = i; break; }
        }
    }

    g_hda.corb_rirb_ready = hda_init_corb_rirb();
    return true;
}

bool hda_enable_interrupts(void) {
    if (!g_hda.present || !g_hda.corb_rirb_ready) return false;

    uint8_t rirbctl = hda_mmio_read8(HDA_REG_RIRBCTL);
    rirbctl |= HDA_RIRBCTL_IRQ | HDA_RIRBCTL_OVF | HDA_RIRBCTL_RUN;
    hda_mmio_write8(HDA_REG_RIRBCTL, rirbctl);

    uint32_t intctl = hda_mmio_read32(HDA_REG_INTCTL);
    intctl |= HDA_INTCTL_GIE;
    hda_mmio_write32(HDA_REG_INTCTL, intctl);
    return true;
}

uint8_t hda_get_irq_line(void) {
    if (!g_hda.present) return 0xFF;
    return pci_config_read8(g_hda.pci.bus, g_hda.pci.slot, g_hda.pci.func, 0x3C);
}

void hda_irq_handler(uint64_t* interrupt_rsp) {
    (void)interrupt_rsp;
    if (!g_hda.present) return;

    uint8_t sts = hda_mmio_read8(HDA_REG_RIRBSTS);
    if (sts & HDA_RIRBSTS_OVF) {
        hda_mmio_write8(HDA_REG_RIRBSTS, HDA_RIRBSTS_OVF);
    }
    if (sts & HDA_RIRBSTS_INT) {
        hda_mmio_write8(HDA_REG_RIRBSTS, HDA_RIRBSTS_INT);
    }
}

bool hda_is_present(void) {
    return g_hda.present;
}

uint16_t hda_get_gcap(void) { return g_hda.gcap; }
uint8_t  hda_get_version_major(void) { return g_hda.vmaj; }
uint8_t  hda_get_version_minor(void) { return g_hda.vmin; }
bool     hda_controller_was_reset(void) { return g_hda.reset_ok; }
bool     hda_has_codec(void) { return g_hda.codec_mask != 0; }
uint8_t  hda_get_primary_codec_id(void) { return g_hda.primary_codec; }
uint16_t hda_get_codec_mask(void) { return g_hda.codec_mask; }
bool     hda_corb_rirb_ready(void) { return g_hda.corb_rirb_ready; }

bool hda_get_codec0_vendor_immediate(uint32_t* out_vendor) {
    if (!g_hda.present) return false;
    return hda_send_verb_immediate(g_hda.primary_codec, 0, HDA_VERB_GET_PARAMETER, 0x00, out_vendor);
}

bool hda_codec0_get_parameter(uint8_t nid, uint16_t parameter, uint32_t* out_resp) {
    if (!g_hda.present) return false;
    return hda_send_verb_immediate(g_hda.primary_codec, nid, HDA_VERB_GET_PARAMETER, parameter, out_resp);
}

bool hda_codec0_get_sub_nodes(uint8_t parent_nid, uint8_t* out_start, uint8_t* out_count) {
    uint32_t resp = 0;
    if (!hda_codec0_get_parameter(parent_nid, HDA_PARAM_NODE_COUNT, &resp)) return false;

    if (out_start) *out_start = (uint8_t)((resp >> 16) & 0xFF);
    if (out_count) *out_count = (uint8_t)(resp & 0xFF);
    return true;
}

bool hda_codec0_set_power_state(uint8_t nid, uint8_t target_state, uint8_t* out_state) {
    uint32_t resp = 0;
    if (!hda_send_verb_immediate(g_hda.primary_codec, nid, HDA_VERB_SET_POWER_STATE, target_state, &resp)) {
        return false;
    }
    if (out_state) *out_state = (uint8_t)(resp & 0x0F);
    return true;
}

// The remaining helpers are placeholders in the simplified port. They keep the
// public API stable for the rest of the kernel but return false to indicate the
// feature is not currently implemented.
bool hda_codec0_find_output_path(uint8_t* out_afg_nid, uint8_t* out_dac_nid, uint8_t* out_pin_nid) {
    (void)out_afg_nid; (void)out_dac_nid; (void)out_pin_nid; return false;
}

bool hda_codec0_power_output_path(uint8_t afg_nid, uint8_t dac_nid, uint8_t pin_nid) {
    (void)afg_nid; (void)dac_nid; (void)pin_nid; return false;
}

bool hda_codec0_configure_output_path(uint8_t dac_nid, uint8_t pin_nid) {
    (void)dac_nid; (void)pin_nid; return false;
}

bool hda_start_output_playback(void) { return false; }

