// src/drivers/hda.c

#include "drivers/hda.h"
#include "drivers/pci.h"
#include "drivers/timer.h"
#include "memory/vmm.h"   // phys_to_virt, vmm_get_kernel_page_table
#include "memory/pmm.h"   // pmm_alloc
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


/* Optional PWT/PCD flags (x86-64) for uncached MMIO,
 * duplicated from AHCI driver for HDA use. */
#ifndef PAGE_PWT
#define PAGE_PWT (1u << 3)
#endif
#ifndef PAGE_PCD
#define PAGE_PCD (1u << 4)
#endif
#ifndef PAGE_PRESENT
#define PAGE_PRESENT (1u << 0)
#endif
#ifndef PAGE_WRITE
#define PAGE_WRITE (1u << 1)
#endif

/* Map a physical MMIO range into the kernel's virtual address space
 * using the higher-half direct map plus explicit page table entries
 * with uncached attributes (PWT|PCD).
 */
static void* hda_map_mmio_uncached(uint64_t phys, size_t size) {
    page_table_t* kpt = vmm_get_kernel_page_table();
    if (!kpt) return NULL;

    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base_va = (uint64_t)phys_to_virt(phys);

    for (size_t i = 0; i < pages; ++i) {
        uint64_t pa = phys + i * PAGE_SIZE;
        uint64_t va = base_va + i * PAGE_SIZE;
        /* NOTE: vmm_map_page already ORs PAGE_PRESENT,
         * but we pass it here as well for consistency with AHCI. */
        vmm_map_page(kpt, va, pa, PAGE_PRESENT | PAGE_WRITE | PAGE_PWT | PAGE_PCD);
    }

    return (void*)base_va;
}

// PCI class codes for High Definition Audio
#define HDA_PCI_CLASS    0x04  // Multimedia
#define HDA_PCI_SUBCLASS 0x03  // HD Audio

// HDA controller register offsets
#define HDA_REG_GCAP     0x00  // Global Capabilities (16-bit)
#define HDA_REG_VMIN     0x02  // Minor Version (8-bit)
#define HDA_REG_VMAJ     0x03  // Major Version (8-bit)
#define HDA_REG_OUTPAY   0x04  // Output Payload Capability (8-bit)
#define HDA_REG_INPAY    0x06  // Input Payload Capability (8-bit)
#define HDA_REG_GCTL     0x08  // Global Control (32-bit)
#define HDA_REG_WAKEEN   0x0C  // Wake Enable (16-bit)
#define HDA_REG_STATESTS 0x0E  // State Change Status / Codec Presence (16-bit)
#define HDA_REG_GSTS     0x10  // Global Status (16-bit)

// Stream base registers
#define HDA_REG_OUTSTRMPAY 0x18 // Output Stream Payload Capability (16-bit)
#define HDA_REG_INSTRMPAY  0x1A // Input Stream Payload Capability (16-bit)

// CORB (Command Output Ring Buffer) registers
#define HDA_REG_CORBLBASE 0x40  // CORB Lower Base Address (32-bit)
#define HDA_REG_CORBUBASE 0x44  // CORB Upper Base Address (32-bit)
#define HDA_REG_CORBWP    0x48  // CORB Write Pointer (8-bit)
#define HDA_REG_CORBRP    0x4A  // CORB Read Pointer (16-bit)
#define HDA_REG_CORBCTL   0x4C  // CORB Control (8-bit)
#define HDA_REG_CORBSTS   0x4D  // CORB Status (8-bit)
#define HDA_REG_CORBSIZE  0x4E  // CORB Size (8-bit)

// RIRB (Response Input Ring Buffer) registers
#define HDA_REG_RIRBLBASE 0x50 // RIRB Lower Base Address (32-bit)
#define HDA_REG_RIRBUBASE 0x54 // RIRB Upper Base Address (32-bit)
#define HDA_REG_RIRBWP    0x58 // RIRB Write Pointer (16-bit)
#define HDA_REG_RINTCNT   0x5A // Response Interrupt Count (16-bit)
#define HDA_REG_RIRBCTL   0x5C // RIRB Control (8-bit)
#define HDA_REG_RIRBSTS   0x5D // RIRB Status (8-bit)
#define HDA_REG_RIRBSIZE  0x5E // RIRB Size (8-bit)

// Immediate Command registers
#define HDA_REG_IC       0x60  // Immediate Command Output (32-bit)
#define HDA_REG_IR       0x64  // Immediate Response Input (32-bit)
#define HDA_REG_ICS      0x68  // Immediate Command Status (16-bit)

// ICS bits
#define HDA_ICS_ICB      (1u << 0)  // Immediate Command Busy
#define HDA_ICS_IRV      (1u << 1)  // Immediate Response Valid

// GCTL bits
#define HDA_GCTL_CRST    (1u << 0)  // Controller Reset
#define HDA_GCTL_UNSOL   (1u << 8)  // Accept unsolicited responses

// CORBCTL bits
#define HDA_CORBCTL_DMA_EN   (1u << 1)

// RIRBCTL bits
#define HDA_RIRBCTL_DMA_EN   (1u << 0)

// CORBRP bits
#define HDA_CORBRP_RST       (1u << 15)

// RIRBWP bits
#define HDA_RIRBWP_RST       (1u << 15)

// CORBSIZE/RIRBSIZE codes (low 2 bits)
#define HDA_CORB_RIRB_2_ENTRIES   0x0
#define HDA_CORB_RIRB_16_ENTRIES  0x1
#define HDA_CORB_RIRB_256_ENTRIES 0x2

// Common verbs/parameters
#define HDA_VERB_GET_PARAMETER  0xF00

// GetParameter parameter IDs
#define HDA_PARAM_VENDOR_ID     0x00
#define HDA_PARAM_NODE_COUNT    0x04

// Power state verbs / values
#define HDA_VERB_SET_POWER_STATE 0x705
#define HDA_VERB_GET_POWER_STATE 0xF05

// HDA controller state
typedef struct {
    bool     present;
    uintptr_t mmio_base;       // MMIO base (kernel virtual address)

    // Basic capability + version
    uint16_t gcap;
    uint8_t  vmaj;
    uint8_t  vmin;

    bool     reset_ok;

    // Codec presence
    uint16_t codec_mask;       // raw STATESTS bits
    bool     codec_present;    // at least one codec?
    uint8_t  primary_codec;    // which codec index (0..14) we chose as "primary"

    // CORB / RIRB
    bool      corb_rirb_ok;
    uint32_t* corb_virt;       // virtual address of CORB DMA buffer
    uint64_t* rirb_virt;       // virtual address of RIRB DMA buffer

    uint16_t corb_entries;     // number of entries (16)
    uint16_t rirb_entries;     // number of entries (16)

    uint8_t  corb_wp;          // last written index
    uint16_t rirb_rp;          // last seen read pointer
} hda_controller_t;

static hda_controller_t g_hda = {0};


// -------------------- Timing helpers --------------------

static uint64_t hda_ticks_from_ms(uint32_t ms) {
    uint32_t freq = timer_get_frequency();
    if (freq == 0) {
        return 1;
    }
    uint64_t ticks = ((uint64_t)freq * (uint64_t)ms + 999) / 1000;
    if (ticks == 0) ticks = 1;
    return ticks;
}

static uint64_t hda_deadline_ms(uint32_t ms) {
    return timer_get_ticks() + hda_ticks_from_ms(ms);
}


// -------------------- MMIO helpers --------------------

static inline uint8_t hda_mmio_read8(size_t offset) {
    return *(volatile uint8_t *)(g_hda.mmio_base + offset);
}

static inline uint16_t hda_mmio_read16(size_t offset) {
    return *(volatile uint16_t *)(g_hda.mmio_base + offset);
}

static inline uint32_t hda_mmio_read32(size_t offset) {
    return *(volatile uint32_t *)(g_hda.mmio_base + offset);
}

static inline void hda_mmio_write8(size_t offset, uint8_t value) {
    *(volatile uint8_t *)(g_hda.mmio_base + offset) = value;
}

static inline void hda_mmio_write16(size_t offset, uint16_t value) {
    *(volatile uint16_t *)(g_hda.mmio_base + offset) = value;
}

static inline void hda_mmio_write32(size_t offset, uint32_t value) {
    *(volatile uint32_t *)(g_hda.mmio_base + offset) = value;
}


// -------------------- Immediate Command (IC) helper --------------------

// Construct a verb for Immediate Command interface
static inline uint32_t hda_make_verb(uint8_t codec, uint8_t node, uint16_t verb, uint16_t payload) {
    return ((uint32_t)codec  << 28)
         | ((uint32_t)node   << 20)
         | ((uint32_t)verb   << 8)
         | ((uint32_t)payload & 0xFF);
}

// Send a single verb via Immediate Command interface and wait for response.
static bool hda_send_verb_immediate(uint8_t codec, uint8_t node,
                                    uint16_t verb, uint16_t payload,
                                    uint32_t* out_resp) {
    if (!g_hda.present || !g_hda.mmio_base) return false;

    // 1) Wait until controller idle (ICB clear)
    uint64_t deadline_idle = hda_deadline_ms(100);
    while (1) {
        uint16_t ics = hda_mmio_read16(HDA_REG_ICS);
        if ((ics & HDA_ICS_ICB) == 0)
            break;
        if (timer_get_ticks() > deadline_idle)
            return false; // timeout waiting for idle
        __asm__ volatile("pause");
    }

    // 2) Write verb to IC
    uint32_t icw = hda_make_verb(codec, node, verb, payload);
    hda_mmio_write32(HDA_REG_IC, icw);

    // 3) Set ICB
    uint16_t ics = hda_mmio_read16(HDA_REG_ICS);
    ics |= HDA_ICS_ICB;
    hda_mmio_write16(HDA_REG_ICS, ics);

    // 4) Poll for IRV (response valid)
    uint64_t deadline_resp = hda_deadline_ms(100);
    while (1) {
        ics = hda_mmio_read16(HDA_REG_ICS);
        if (ics & HDA_ICS_IRV)
            break;
        if (timer_get_ticks() > deadline_resp)
            return false; // timeout waiting for response
        __asm__ volatile("pause");
    }

    // 5) Read response and clear IRV
    uint32_t resp = hda_mmio_read32(HDA_REG_IR);

    ics = hda_mmio_read16(HDA_REG_ICS);
    ics &= ~HDA_ICS_IRV;
    hda_mmio_write16(HDA_REG_ICS, ics);

    if (out_resp)
        *out_resp = resp;

    return true;
}


// -------------------- Controller reset & codec discovery --------------------

static bool hda_reset_controller(void) {
    if (!g_hda.present || !g_hda.mmio_base) return false;

    // 1) Put controller into reset
    uint32_t gctl = hda_mmio_read32(HDA_REG_GCTL);
    gctl &= ~HDA_GCTL_CRST;
    hda_mmio_write32(HDA_REG_GCTL, gctl);

    // 2) Wait for hardware to clear CRST
    uint64_t deadline_release = hda_deadline_ms(100);
    while (1) {
        gctl = hda_mmio_read32(HDA_REG_GCTL);
        if ((gctl & HDA_GCTL_CRST) == 0)
            break;
        if (timer_get_ticks() > deadline_release)
            return false;
        __asm__ volatile("pause");
    }

    // 3) Set CRST to bring controller out of reset
    gctl |= HDA_GCTL_CRST;
    hda_mmio_write32(HDA_REG_GCTL, gctl);

    // 4) Wait for hardware to set CRST
    uint64_t deadline_crst = hda_deadline_ms(100);
    while (1) {
        gctl = hda_mmio_read32(HDA_REG_GCTL);
        if (gctl & HDA_GCTL_CRST)
            break;
        if (timer_get_ticks() > deadline_crst)
            return false;
        __asm__ volatile("pause");
    }

    return true;
}

static void hda_discover_codecs(void) {
    if (!g_hda.present || !g_hda.mmio_base) return;

    // STATESTS bits [0..14] indicate codec presence
    uint16_t mask = hda_mmio_read16(HDA_REG_STATESTS);
    g_hda.codec_mask = mask;

    if (mask == 0) {
        g_hda.codec_present = false;
        return;
    }

    g_hda.codec_present = true;

    // Choose the lowest-index codec bit as "primary".
    for (uint8_t c = 0; c < 15; c++) {
        if (mask & (1u << c)) {
            g_hda.primary_codec = c;
            return;
        }
    }

    // Fallback in case something weird happens
    g_hda.primary_codec = 0;
}


// -------------------- CORB/RIRB init --------------------

// For now: 16 entries for CORB and 16 for RIRB,
// one 4KB page each from the PMM, DMA enabled, polled usage.

static bool hda_init_corb_rirb(void) {
    if (!g_hda.present || !g_hda.mmio_base) return false;

    // 1) Disable CORB/RIRB DMA while we configure
    uint8_t corbctl = hda_mmio_read8(HDA_REG_CORBCTL);
    corbctl &= ~HDA_CORBCTL_DMA_EN;
    hda_mmio_write8(HDA_REG_CORBCTL, corbctl);

    uint8_t rirbctl = hda_mmio_read8(HDA_REG_RIRBCTL);
    rirbctl &= ~HDA_RIRBCTL_DMA_EN;
    hda_mmio_write8(HDA_REG_RIRBCTL, rirbctl);

    // 2) Allocate one page each for CORB and RIRB from the PMM.
    void* corb_page = pmm_alloc();
    if (!corb_page) return false;
    uint64_t corb_phys = (uint64_t)(uintptr_t)corb_page;

    void* rirb_page = pmm_alloc();
    if (!rirb_page) return false;
    uint64_t rirb_phys = (uint64_t)(uintptr_t)rirb_page;

    // CORB/RIRB live in normal RAM; use HHDM to get virtual addresses.
    g_hda.corb_virt = (uint32_t*)phys_to_virt(corb_phys);
    g_hda.rirb_virt = (uint64_t*)phys_to_virt(rirb_phys);

    g_hda.corb_entries = 16;
    g_hda.rirb_entries = 16;
    g_hda.corb_wp      = 0;
    g_hda.rirb_rp      = 0;

    // 3) Program CORB/RIRB base registers
    hda_mmio_write32(HDA_REG_CORBLBASE, (uint32_t)(corb_phys & 0xFFFFFFFFu));
    hda_mmio_write32(HDA_REG_CORBUBASE, (uint32_t)(corb_phys >> 32));

    hda_mmio_write32(HDA_REG_RIRBLBASE, (uint32_t)(rirb_phys & 0xFFFFFFFFu));
    hda_mmio_write32(HDA_REG_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    // 4) Choose 16-entry size for both rings.
    hda_mmio_write8(HDA_REG_CORBSIZE, HDA_CORB_RIRB_16_ENTRIES);
    hda_mmio_write8(HDA_REG_RIRBSIZE, HDA_CORB_RIRB_16_ENTRIES);

    // 5) Reset CORB/RIRB pointers
    hda_mmio_write16(HDA_REG_CORBRP, HDA_CORBRP_RST);
    hda_mmio_write16(HDA_REG_RIRBWP, HDA_RIRBWP_RST);

    // 6) Enable CORB and RIRB DMA
    corbctl = hda_mmio_read8(HDA_REG_CORBCTL);
    corbctl |= HDA_CORBCTL_DMA_EN;
    hda_mmio_write8(HDA_REG_CORBCTL, corbctl);

    rirbctl = hda_mmio_read8(HDA_REG_RIRBCTL);
    rirbctl |= HDA_RIRBCTL_DMA_EN;
    hda_mmio_write8(HDA_REG_RIRBCTL, rirbctl);

    g_hda.corb_rirb_ok = true;
    return true;
}


// -------------------- CORB/RIRB verb send (polled) --------------------

static bool hda_send_verb_corb(uint8_t codec, uint8_t node,
                               uint16_t verb, uint16_t payload,
                               uint32_t* out_resp) {
    if (!g_hda.present || !g_hda.mmio_base) return false;
    if (!g_hda.corb_rirb_ok) return false;
    if (!g_hda.corb_virt || !g_hda.rirb_virt) return false;

    uint16_t corb_entries = g_hda.corb_entries;
    uint16_t rirb_entries = g_hda.rirb_entries;

    if (corb_entries == 0 || rirb_entries == 0)
        return false;

    // 1) Compute next CORB index and write the verb there
    uint8_t new_wp = (uint8_t)((g_hda.corb_wp + 1) & (corb_entries - 1));
    uint32_t cmd   = hda_make_verb(codec, node, verb, payload);

    g_hda.corb_virt[new_wp] = cmd;
    g_hda.corb_wp = new_wp;

    // 2) Notify hardware of new write pointer
    hda_mmio_write8(HDA_REG_CORBWP, new_wp);

    // 3) Poll RIRBWP until it advances
    uint16_t old_rp = g_hda.rirb_rp;

    uint64_t deadline_rirb = hda_deadline_ms(100);
    while (1) {
        uint16_t hw_wp = hda_mmio_read16(HDA_REG_RIRBWP) & 0xFFu;
        if (hw_wp != old_rp) {
            // New response available at index hw_wp
            g_hda.rirb_rp = hw_wp;
            uint16_t idx  = (uint16_t)(hw_wp & (rirb_entries - 1));

            uint64_t entry = g_hda.rirb_virt[idx];
            uint32_t resp  = (uint32_t)(entry & 0xFFFFFFFFu);

            // Clear any RIRB status bits by writing back what we read
            uint8_t sts = hda_mmio_read8(HDA_REG_RIRBSTS);
            if (sts) {
                hda_mmio_write8(HDA_REG_RIRBSTS, sts);
            }

            if (out_resp)
                *out_resp = resp;
            return true;
        }

        if (timer_get_ticks() > deadline_rirb)
            break;

        __asm__ volatile("pause");
    }

    return false; // timeout waiting for RIRB response
}

// Try CORB first (if configured) and fall back to Immediate Command on failure.
static bool hda_send_verb_best_available(uint8_t codec, uint8_t node,
                                         uint16_t verb, uint16_t payload,
                                         uint32_t* out_resp) {
    if (g_hda.corb_rirb_ok) {
        if (hda_send_verb_corb(codec, node, verb, payload, out_resp)) {
            return true;
        }
    }

    return hda_send_verb_immediate(codec, node, verb, payload, out_resp);
}


// -------------------- Public API: init --------------------

bool hda_init(void) {
    pci_device_t dev;

    // 1) PCI detect
    if (!pci_find_class_subclass(HDA_PCI_CLASS, HDA_PCI_SUBCLASS, &dev)) {
        g_hda.present = false;
        return false;
    }

    // 2) Enable MMIO + bus mastering for this controller
    pci_enable_mmio_and_bus_mastering(dev.bus, dev.slot, dev.func);

    // 3) Read BAR0 (MMIO base)
    uint32_t bar0 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x10);
    if (bar0 & 1) { // IO-space BAR? shouldn't happen for HDA
        g_hda.present = false;
        return false;
    }

    uintptr_t phys_base = (uintptr_t)(bar0 & ~0xFu);
    if (!phys_base) {
        g_hda.present = false;
        return false;
    }

    // Map MMIO BAR into the kernel's address space as uncached MMIO.
    // Intel HDA typically exposes up to 16KiB of registers; 0x4000 is safe.
    void* mmio = hda_map_mmio_uncached((uint64_t)phys_base, 0x4000);
    if (!mmio) {
        g_hda.present = false;
        return false;
    }

    g_hda.mmio_base = (uintptr_t)mmio;
    g_hda.present   = true;

    // 3) Read GCAP, version
    g_hda.gcap = hda_mmio_read16(HDA_REG_GCAP);
    g_hda.vmin = hda_mmio_read8(HDA_REG_VMIN);
    g_hda.vmaj = hda_mmio_read8(HDA_REG_VMAJ);

    // 4) Reset controller
    g_hda.reset_ok = hda_reset_controller();
    if (!g_hda.reset_ok) {
        g_hda.present = false;
        return false;
    }

    // 5) Discover codecs
    hda_discover_codecs();
    if (!g_hda.codec_present) {
        return false;
    }

    // 6) Init CORB/RIRB
    if (!hda_init_corb_rirb()) {
        g_hda.corb_rirb_ok = false;
    }

    return true;
}


// -------------------- Helpers: codec0 info --------------------

bool hda_get_codec0_vendor_immediate(uint32_t* out_vendor) {
    if (!g_hda.present || !g_hda.mmio_base) return false;
    if (!g_hda.codec_present) return false;
    if (!g_hda.codec_mask) return false;

    uint16_t mask = g_hda.codec_mask;
    uint32_t resp = 0;

    for (uint8_t c = 0; c < 15; c++) {
        if (!(mask & (1u << c)))
            continue;

        if (hda_send_verb_best_available(c, 0,
                                         HDA_VERB_GET_PARAMETER,
                                         HDA_PARAM_VENDOR_ID,
                                         &resp)) {
            if (out_vendor)
                *out_vendor = resp;
            g_hda.primary_codec = c;
            return true;
        }
    }

    return false;
}

// Query the sub-nodes of a parent node on the primary codec using GetParameter(NodeCount).
bool hda_codec0_get_sub_nodes(uint8_t parent_nid,
                              uint8_t* out_start,
                              uint8_t* out_count) {
    if (!g_hda.present || !g_hda.mmio_base) return false;
    if (!g_hda.codec_present) return false;
    if (!g_hda.codec_mask) return false;

    uint8_t codec = g_hda.primary_codec;
    uint32_t resp = 0;

    if (!hda_send_verb_best_available(codec, parent_nid,
                                      HDA_VERB_GET_PARAMETER,
                                      HDA_PARAM_NODE_COUNT,
                                      &resp)) {
        return false;
    }

    uint8_t start = (uint8_t)((resp >> 16) & 0xFF);
    uint8_t count = (uint8_t)(resp & 0xFF);

    if (out_start) *out_start = start;
    if (out_count) *out_count = count;
    return true;
}

bool hda_codec0_get_parameter(uint8_t nid, uint16_t parameter, uint32_t* out_resp) {
    if (!g_hda.present || !g_hda.mmio_base) return false;
    if (!g_hda.codec_present) return false;
    if (!g_hda.codec_mask) return false;

    uint8_t codec = g_hda.primary_codec;
    return hda_send_verb_best_available(codec, nid,
                                        HDA_VERB_GET_PARAMETER,
                                        parameter,
                                        out_resp);
}

bool hda_codec0_set_power_state(uint8_t nid, uint8_t target_state, uint8_t* out_state) {
    if (!g_hda.present || !g_hda.mmio_base) return false;
    if (!g_hda.codec_present) return false;
    if (!g_hda.codec_mask) return false;

    uint8_t codec = g_hda.primary_codec;
    uint32_t resp = 0;

    if (!hda_send_verb_best_available(codec, nid,
                                      HDA_VERB_SET_POWER_STATE,
                                      target_state,
                                      &resp)) {
        return false;
    }

    uint8_t current = (uint8_t)(resp & 0x0F);
    if (out_state) {
        *out_state = current;
    }

    return true;
}


// -------------------- Simple public accessors --------------------

bool hda_is_present(void) {
    return g_hda.present;
}

uint16_t hda_get_gcap(void) {
    return g_hda.gcap;
}

uint8_t hda_get_version_major(void) {
    return g_hda.vmaj;
}

uint8_t hda_get_version_minor(void) {
    return g_hda.vmin;
}

bool hda_controller_was_reset(void) {
    return g_hda.reset_ok;
}
bool hda_has_codec(void) {
    return g_hda.codec_present;
}
uint8_t hda_get_primary_codec_id(void) {
    return g_hda.primary_codec;
}
uint16_t hda_get_codec_mask(void) {
    return g_hda.codec_mask;
}
bool hda_corb_rirb_ready(void) {
    return g_hda.corb_rirb_ok;
}
