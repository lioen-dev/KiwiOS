// src/drivers/hda.c

#include "drivers/hda.h"
#include "drivers/pci.h"
#include "drivers/timer.h"
#include "memory/vmm.h"   // phys_to_virt, vmm_get_kernel_page_table
#include "memory/pmm.h"   // pmm_alloc
#include "arch/x86/io.h"  // outb
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>


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

// Interrupt control/status
#define HDA_REG_INTCTL    0x20 // Interrupt Control (32-bit)
#define HDA_REG_INTSTS    0x24 // Interrupt Status (32-bit)
#define HDA_INTCTL_GIE    (1u << 31)

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
#define HDA_RIRBCTL_IRQ_EN   (1u << 1)
#define HDA_RIRBCTL_OVF_EN   (1u << 2)

// CORBRP bits
#define HDA_CORBRP_RST       (1u << 15)

// RIRBWP bits
#define HDA_RIRBWP_RST       (1u << 15)

// CORBSIZE/RIRBSIZE codes (low 2 bits)
#define HDA_CORB_RIRB_2_ENTRIES   0x0
#define HDA_CORB_RIRB_16_ENTRIES  0x1
#define HDA_CORB_RIRB_256_ENTRIES 0x2

// RIRBSTS bits
#define HDA_RIRBSTS_IRQ      (1u << 0)
#define HDA_RIRBSTS_OVF      (1u << 2)

// Common verbs/parameters
#define HDA_VERB_GET_PARAMETER  0xF00
#define HDA_VERB_SET_CONVERTER_FORMAT 0x200
#define HDA_VERB_SET_CONVERTER_STREAM 0x706
#define HDA_VERB_SET_PIN_WIDGET_CONTROL 0x707

// GetParameter parameter IDs
#define HDA_PARAM_VENDOR_ID     0x00
#define HDA_PARAM_NODE_COUNT    0x04
#define HDA_PARAM_FUNCTION_TYPE 0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP 0x09
#define HDA_PARAM_PIN_CAP       0x0C

// Power state verbs / values
#define HDA_VERB_SET_POWER_STATE 0x705
#define HDA_VERB_GET_POWER_STATE 0xF05
#define HDA_VERB_SET_EAPD_BTLENABLE 0x70C

// Function Group Type values (from GetParameter FunctionGroupType)
#define HDA_FG_TYPE_AUDIO 0x01

// Audio Widget Capability bits
#define HDA_AWCAP_OUTPUT      (1u << 4)
#define HDA_AWCAP_WIDGET_TYPE_SHIFT 20
#define HDA_AWCAP_WIDGET_TYPE_MASK  0xF

// Widget types (from Audio Widget Capabilities bits [23:20])
#define HDA_WIDGET_TYPE_AUDIO_OUTPUT 0x0
#define HDA_WIDGET_TYPE_AUDIO_INPUT  0x1
#define HDA_WIDGET_TYPE_PIN_COMPLEX  0x4

// Pin capabilities (from GetParameter PinCapabilities, bit 4 == output)
#define HDA_PINCAP_OUTPUT       (1u << 4)

#define HDA_PIN_WIDGET_CONTROL_OUT 0x40
#define HDA_EAPD_BTL_ENABLE        0x02

// Stream descriptor offsets and bits
#define HDA_REG_SD_BASE         0x80
#define HDA_REG_SD_INTERVAL     0x20

#define HDA_SD_CTL              0x00
#define HDA_SD_STS              0x03
#define HDA_SD_LPIB             0x04
#define HDA_SD_CBL              0x08
#define HDA_SD_LVI              0x0C
#define HDA_SD_FMT              0x12
#define HDA_SD_BDPL             0x18
#define HDA_SD_BDPU             0x1C

#define HDA_SDCTL_SRST          (1u << 0)
#define HDA_SDCTL_RUN           (1u << 1)
#define HDA_SDCTL_IOCE          (1u << 2)
#define HDA_SDCTL_FEIE          (1u << 3)
#define HDA_SDCTL_DEIE          (1u << 4)
#define HDA_SDCTL_STRIPE_MASK   (3u << 16)
#define HDA_SDCTL_TRAFFIC_PRIO  (1u << 18)
#define HDA_SDCTL_STREAM_TAG_SHIFT 20
#define HDA_SDCTL_STREAM_TAG_MASK  (0xFu << HDA_SDCTL_STREAM_TAG_SHIFT)

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

    // Interrupt / response handling
    uint8_t  irq_line;         // PCI legacy IRQ line (0xFF if none)
    bool     irq_enabled;      // PIC/IDT wired up
    bool     rirb_irq_armed;   // RIRB interrupt enable programmed

    uint64_t resp_queue[32];   // buffered RIRB responses (low 32 bits used)
    uint8_t  resp_head;        // dequeue index
    uint8_t  resp_tail;        // enqueue index

    // Cached discovery info for a simple playback path
    uint8_t  afg_nid;          // first audio function group
    uint8_t  out_dac_nid;      // first audio output converter (DAC)
    uint8_t  out_pin_nid;      // first pin complex advertising output
    bool     playback_path_cached;

    // Simple playback stream state
    uint8_t  playback_stream_index; // zero-based stream descriptor index
    uint8_t  playback_stream_tag;   // hardware stream tag programmed into SDnCTL
    bool     playback_stream_ready;
    uint64_t playback_bdl_phys;
    uint64_t playback_buffer_phys;
    size_t   playback_buffer_bytes;
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

// -------------------- Stream helpers --------------------

typedef struct {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed)) hda_bdl_entry_t;

static inline uint8_t hda_output_stream_count(void) {
    return (uint8_t)((g_hda.gcap & 0x0Fu) + 1);
}

static inline size_t hda_stream_offset(uint8_t index) {
    return HDA_REG_SD_BASE + (size_t)index * HDA_REG_SD_INTERVAL;
}

static bool hda_reset_stream(uint8_t index) {
    size_t off = hda_stream_offset(index);
    uint32_t ctl = hda_mmio_read32(off + HDA_SD_CTL);

    // Assert stream reset
    ctl |= HDA_SDCTL_SRST;
    hda_mmio_write32(off + HDA_SD_CTL, ctl);

    uint64_t deadline = hda_deadline_ms(100);
    while ((hda_mmio_read32(off + HDA_SD_CTL) & HDA_SDCTL_SRST) == 0) {
        if (timer_get_ticks() > deadline) {
            return false;
        }
        __asm__ volatile("pause");
    }

    // De-assert stream reset
    ctl &= ~HDA_SDCTL_SRST;
    hda_mmio_write32(off + HDA_SD_CTL, ctl);

    deadline = hda_deadline_ms(100);
    while (hda_mmio_read32(off + HDA_SD_CTL) & HDA_SDCTL_SRST) {
        if (timer_get_ticks() > deadline) {
            return false;
        }
        __asm__ volatile("pause");
    }

    // Clear any sticky status bits
    hda_mmio_write8(off + HDA_SD_STS, 0xFF);
    return true;
}

// -------------------- Response queue helpers --------------------

static inline void hda_resp_queue_reset(void) {
    g_hda.resp_head = 0;
    g_hda.resp_tail = 0;
}

static inline void hda_resp_queue_push(uint64_t entry) {
    uint8_t next_tail = (uint8_t)((g_hda.resp_tail + 1) & 0x1F);
    if (next_tail == g_hda.resp_head) {
        // Drop oldest entry on overflow
        g_hda.resp_head = (uint8_t)((g_hda.resp_head + 1) & 0x1F);
    }

    g_hda.resp_queue[g_hda.resp_tail] = entry;
    g_hda.resp_tail = next_tail;
}

static inline bool hda_resp_queue_pop(uint32_t* out_resp) {
    if (g_hda.resp_head == g_hda.resp_tail)
        return false;

    uint64_t entry = g_hda.resp_queue[g_hda.resp_head];
    g_hda.resp_head = (uint8_t)((g_hda.resp_head + 1) & 0x1F);

    if (out_resp)
        *out_resp = (uint32_t)(entry & 0xFFFFFFFFu);
    return true;
}

static bool hda_wait_for_rirb_irq(uint8_t start_tail, uint32_t* out_resp, uint32_t timeout_ms) {
    uint64_t deadline = hda_deadline_ms(timeout_ms);
    while (g_hda.resp_tail == start_tail) {
        if (timer_get_ticks() > deadline)
            return false;
        __asm__ volatile("pause");
    }

    return hda_resp_queue_pop(out_resp);
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

    // STATESTS bits [0..14] indicate codec presence. On some real hardware the
    // bits can take a little longer to assert after controller reset, so give
    // them a modest grace period before concluding nothing is present.
    uint64_t deadline = hda_deadline_ms(500);
    uint16_t mask = 0;
    while (timer_get_ticks() <= deadline) {
        mask = hda_mmio_read16(HDA_REG_STATESTS) & 0x7FFFu;
        if (mask != 0) break;
        __asm__ volatile("pause");
    }

    g_hda.codec_mask = mask;

    // Clear any latched presence-change bits that may have accumulated during
    // reset so subsequent hotplug/change events are visible.
    if (mask) {
        hda_mmio_write16(HDA_REG_STATESTS, mask);
    }

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
    if (!rirb_page) {
        pmm_free(corb_page);
        return false;
    }
    uint64_t rirb_phys = (uint64_t)(uintptr_t)rirb_page;

    // CORB/RIRB live in normal RAM; use HHDM to get virtual addresses.
    g_hda.corb_virt = (uint32_t*)phys_to_virt(corb_phys);
    g_hda.rirb_virt = (uint64_t*)phys_to_virt(rirb_phys);

    memset(g_hda.corb_virt, 0, PAGE_SIZE);
    memset(g_hda.rirb_virt, 0, PAGE_SIZE);

    // 2a) Determine supported CORB/RIRB sizes and pick the largest common option
    //     (prefer 256 > 16 > 2 entries).
    uint8_t corbsize_cap = hda_mmio_read8(HDA_REG_CORBSIZE);
    uint8_t rirbsize_cap = hda_mmio_read8(HDA_REG_RIRBSIZE);

    struct {
        uint8_t code;
        uint16_t entries;
        uint8_t cap_bit;
    } size_options[] = {
        {HDA_CORB_RIRB_256_ENTRIES, 256, 6},
        {HDA_CORB_RIRB_16_ENTRIES,  16, 5},
        {HDA_CORB_RIRB_2_ENTRIES,    2, 4},
    };

    uint8_t chosen_code = 0;
    uint16_t chosen_entries = 0;

    for (size_t i = 0; i < sizeof(size_options) / sizeof(size_options[0]); ++i) {
        bool corb_ok = corbsize_cap & (1u << size_options[i].cap_bit);
        bool rirb_ok = rirbsize_cap & (1u << size_options[i].cap_bit);
        if (corb_ok && rirb_ok) {
            chosen_code = size_options[i].code;
            chosen_entries = size_options[i].entries;
            break;
        }
    }

    // If no common size bits were set, fall back to 16 entries (spec default).
    if (chosen_entries == 0) {
        chosen_code = HDA_CORB_RIRB_16_ENTRIES;
        chosen_entries = 16;
    }

    g_hda.corb_entries = chosen_entries;
    g_hda.rirb_entries = chosen_entries;
    g_hda.corb_wp      = 0;
    g_hda.rirb_rp      = 0;
    hda_resp_queue_reset();

    // 3) Program CORB/RIRB base registers
    hda_mmio_write32(HDA_REG_CORBLBASE, (uint32_t)(corb_phys & 0xFFFFFFFFu));
    hda_mmio_write32(HDA_REG_CORBUBASE, (uint32_t)(corb_phys >> 32));

    hda_mmio_write32(HDA_REG_RIRBLBASE, (uint32_t)(rirb_phys & 0xFFFFFFFFu));
    hda_mmio_write32(HDA_REG_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    // 4) Program the negotiated size for both rings.
    hda_mmio_write8(HDA_REG_CORBSIZE, chosen_code);
    hda_mmio_write8(HDA_REG_RIRBSIZE, chosen_code);

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

    // Ensure we start from a clean queue snapshot when using interrupts.
    if (g_hda.rirb_irq_armed) {
        hda_resp_queue_reset();
        g_hda.rirb_rp = hda_mmio_read16(HDA_REG_RIRBWP) & 0xFFu;
    }

    g_hda.corb_virt[new_wp] = cmd;
    g_hda.corb_wp = new_wp;

    // 2) Notify hardware of new write pointer
    hda_mmio_write8(HDA_REG_CORBWP, new_wp);

    // 3) Prefer interrupt-backed completion if available
    if (g_hda.rirb_irq_armed) {
        uint8_t start_tail = g_hda.resp_tail;
        if (hda_wait_for_rirb_irq(start_tail, out_resp, 100)) {
            return true;
        }
        // Fall back to polling if the interrupt never arrived
    }

    // 4) Poll RIRBWP until it advances
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

    g_hda.irq_line       = 0xFF;
    g_hda.irq_enabled    = false;
    g_hda.rirb_irq_armed = false;
    g_hda.playback_path_cached = false;
    g_hda.afg_nid       = 0;
    g_hda.out_dac_nid   = 0;
    g_hda.out_pin_nid   = 0;
    g_hda.playback_stream_index = 0;
    g_hda.playback_stream_tag   = 0;
    g_hda.playback_stream_ready = false;
    g_hda.playback_bdl_phys     = 0;
    g_hda.playback_buffer_phys  = 0;
    g_hda.playback_buffer_bytes = 0;
    hda_resp_queue_reset();

    // 1) PCI detect
    if (!pci_find_class_subclass(HDA_PCI_CLASS, HDA_PCI_SUBCLASS, &dev)) {
        g_hda.present = false;
        return false;
    }

    g_hda.irq_line = pci_config_read8(dev.bus, dev.slot, dev.func, 0x3C);

    // 2) Enable MMIO + bus mastering for this controller
    pci_enable_mmio_and_bus_mastering(dev.bus, dev.slot, dev.func);

    // 3) Read BAR0/BAR1 (MMIO base)
    uint32_t bar0 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x10);
    if (bar0 & 1) { // IO-space BAR? shouldn't happen for HDA
        g_hda.present = false;
        return false;
    }

    bool bar0_is_64 = ((bar0 >> 1) & 0x3) == 0x2;
    uint32_t bar1 = 0;
    if (bar0_is_64) {
        bar1 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x14);
    }

    uint64_t phys_base = ((uint64_t)bar1 << 32) | (uint64_t)(bar0 & ~0xFu);
    if (!phys_base) {
        g_hda.present = false;
        return false;
    }

    // Map MMIO BAR into the kernel's address space as uncached MMIO.
    // Intel HDA typically exposes up to 16KiB of registers; 0x4000 is safe.
    void* mmio = hda_map_mmio_uncached(phys_base, 0x4000);
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
        // No codecs present is not fatal; keep the controller marked present so
        // other subsystems can continue booting without audio capabilities.
        g_hda.corb_rirb_ok = false;
        return true;
    }

    // 6) Init CORB/RIRB
    if (!hda_init_corb_rirb()) {
        g_hda.corb_rirb_ok = false;
    }

    return true;
}

bool hda_enable_interrupts(void) {
    if (!g_hda.present || !g_hda.mmio_base) return false;
    if (!g_hda.corb_rirb_ok) return false;

    uint8_t rirbctl = hda_mmio_read8(HDA_REG_RIRBCTL);
    rirbctl |= HDA_RIRBCTL_DMA_EN | HDA_RIRBCTL_IRQ_EN | HDA_RIRBCTL_OVF_EN;
    hda_mmio_write8(HDA_REG_RIRBCTL, rirbctl);

    uint8_t sts = hda_mmio_read8(HDA_REG_RIRBSTS);
    if (sts) {
        hda_mmio_write8(HDA_REG_RIRBSTS, sts);
    }

    g_hda.rirb_irq_armed = true;
    g_hda.irq_enabled    = true;
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

void hda_irq_handler(uint64_t* interrupt_rsp) {
    (void)interrupt_rsp;

    if (!g_hda.present || !g_hda.mmio_base) goto ack;
    if (!g_hda.corb_rirb_ok) goto ack;

    uint8_t sts = hda_mmio_read8(HDA_REG_RIRBSTS);
    if ((sts & (HDA_RIRBSTS_IRQ | HDA_RIRBSTS_OVF)) == 0) {
        goto ack;
    }

    uint16_t hw_wp = hda_mmio_read16(HDA_REG_RIRBWP) & 0xFFu;
    uint16_t rp    = g_hda.rirb_rp;

    if (g_hda.rirb_entries == 0 || !g_hda.rirb_virt) {
        goto clear_only;
    }

    while (rp != hw_wp) {
        rp = (uint16_t)((rp + 1) & 0xFFu);
        uint16_t idx = (uint16_t)(rp & (g_hda.rirb_entries - 1));
        uint64_t entry = g_hda.rirb_virt[idx];
        hda_resp_queue_push(entry);
    }

    g_hda.rirb_rp = hw_wp;

clear_only:
    if (sts) {
        hda_mmio_write8(HDA_REG_RIRBSTS, sts);
    }

ack:
    if (g_hda.irq_line >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
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
    uint64_t deadline = hda_deadline_ms(200);

    while (current != target_state) {
        if (timer_get_ticks() > deadline) {
            break;
        }

        uint32_t get_resp = 0;
        if (!hda_send_verb_best_available(codec, nid,
                                          HDA_VERB_GET_POWER_STATE,
                                          0,
                                          &get_resp)) {
            break;
        }

        current = (uint8_t)(get_resp & 0x0F);
        if (current == target_state) {
            break;
        }

        __asm__ volatile("pause");
    }

    if (out_state) {
        *out_state = current;
    }

    return current == target_state;
}


// -------------------- Audio topology helpers --------------------

static bool hda_find_first_audio_function_group(uint8_t* out_afg_nid) {
    uint8_t fg_start = 0;
    uint8_t fg_count = 0;

    if (!hda_codec0_get_sub_nodes(0, &fg_start, &fg_count)) {
        return false;
    }

    for (uint16_t i = 0; i < fg_count; ++i) {
        uint8_t nid = (uint8_t)(fg_start + i);
        uint32_t resp = 0;
        if (!hda_codec0_get_parameter(nid, HDA_PARAM_FUNCTION_TYPE, &resp)) {
            continue;
        }

        uint8_t fg_type = (uint8_t)(resp & 0x7F); // low 7 bits hold the type
        if (fg_type == HDA_FG_TYPE_AUDIO) {
            if (out_afg_nid)
                *out_afg_nid = nid;
            return true;
        }
    }

    return false;
}

bool hda_codec0_find_output_path(uint8_t* out_afg_nid, uint8_t* out_dac_nid, uint8_t* out_pin_nid) {
    if (!g_hda.present || !g_hda.mmio_base) return false;
    if (!g_hda.codec_present) return false;
    if (!g_hda.codec_mask) return false;

    if (g_hda.playback_path_cached) {
        if (g_hda.afg_nid && g_hda.out_dac_nid && g_hda.out_pin_nid) {
            if (out_afg_nid) *out_afg_nid = g_hda.afg_nid;
            if (out_dac_nid) *out_dac_nid = g_hda.out_dac_nid;
            if (out_pin_nid) *out_pin_nid = g_hda.out_pin_nid;
            return true;
        }
        // cached attempt failed previously
        return false;
    }

    uint8_t afg_nid = 0;
    if (!hda_find_first_audio_function_group(&afg_nid)) {
        g_hda.playback_path_cached = true;
        return false;
    }

    uint8_t widget_start = 0;
    uint8_t widget_count = 0;
    if (!hda_codec0_get_sub_nodes(afg_nid, &widget_start, &widget_count)) {
        g_hda.playback_path_cached = true;
        return false;
    }

    uint8_t dac_nid = 0;
    uint8_t pin_nid = 0;

    for (uint16_t i = 0; i < widget_count; ++i) {
        uint8_t nid = (uint8_t)(widget_start + i);
        uint32_t awcap = 0;
        if (!hda_codec0_get_parameter(nid, HDA_PARAM_AUDIO_WIDGET_CAP, &awcap)) {
            continue;
        }

        uint8_t widget_type = (uint8_t)((awcap >> HDA_AWCAP_WIDGET_TYPE_SHIFT) & HDA_AWCAP_WIDGET_TYPE_MASK);

        if (!dac_nid && widget_type == HDA_WIDGET_TYPE_AUDIO_OUTPUT) {
            dac_nid = nid;
        }

        if (!pin_nid && widget_type == HDA_WIDGET_TYPE_PIN_COMPLEX) {
            uint32_t pincap = 0;
            if (!hda_codec0_get_parameter(nid, HDA_PARAM_PIN_CAP, &pincap)) {
                continue;
            }

            if (pincap & HDA_PINCAP_OUTPUT) {
                pin_nid = nid;
            }
        }

        if (dac_nid && pin_nid) {
            break;
        }
    }

    g_hda.afg_nid = afg_nid;
    g_hda.out_dac_nid = dac_nid;
    g_hda.out_pin_nid = pin_nid;
    g_hda.playback_path_cached = true;

    if (out_afg_nid) *out_afg_nid = afg_nid;
    if (out_dac_nid) *out_dac_nid = dac_nid;
    if (out_pin_nid) *out_pin_nid = pin_nid;

    return (dac_nid != 0) && (pin_nid != 0);
}

// Request D0 power for the AFG/DAC/pin nodes involved in playback.
bool hda_codec0_power_output_path(uint8_t afg_nid, uint8_t dac_nid, uint8_t pin_nid) {
    if (!afg_nid || !dac_nid || !pin_nid) return false;

    uint8_t state = 0;

    if (!hda_codec0_set_power_state(afg_nid, HDA_POWER_STATE_D0, &state) || state != HDA_POWER_STATE_D0) {
        return false;
    }

    if (!hda_codec0_set_power_state(dac_nid, HDA_POWER_STATE_D0, &state) || state != HDA_POWER_STATE_D0) {
        return false;
    }

    if (!hda_codec0_set_power_state(pin_nid, HDA_POWER_STATE_D0, &state) || state != HDA_POWER_STATE_D0) {
        return false;
    }

    return true;
}

bool hda_codec0_configure_output_path(uint8_t dac_nid, uint8_t pin_nid) {
    if (!g_hda.present || !g_hda.codec_present || dac_nid == 0 || pin_nid == 0) {
        return false;
    }

    uint32_t resp = 0;
    uint8_t codec = g_hda.primary_codec;

    // Enable output on the discovered pin widget.
    if (!hda_send_verb_best_available(codec, pin_nid,
                                      HDA_VERB_SET_PIN_WIDGET_CONTROL,
                                      HDA_PIN_WIDGET_CONTROL_OUT,
                                      &resp)) {
        return false;
    }

    // Power up external amplifier if present.
    if (!hda_send_verb_best_available(codec, pin_nid,
                                      HDA_VERB_SET_EAPD_BTLENABLE,
                                      HDA_EAPD_BTL_ENABLE,
                                      &resp)) {
        return false;
    }

    (void)dac_nid; // DAC-specific configuration can be added later.

    return true;
}

static bool hda_codec0_set_converter_stream_channel(uint8_t dac_nid, uint8_t stream_tag, uint8_t channel) {
    if (!dac_nid || stream_tag == 0) {
        return false;
    }

    uint8_t codec = g_hda.primary_codec;
    uint32_t resp = 0;
    uint16_t payload = (uint16_t)(((stream_tag & 0x0F) << 4) | (channel & 0x0F));

    return hda_send_verb_best_available(codec, dac_nid,
                                        HDA_VERB_SET_CONVERTER_STREAM,
                                        payload,
                                        &resp);
}

static bool hda_codec0_set_converter_format(uint8_t dac_nid, uint16_t fmt) {
    if (!dac_nid) {
        return false;
    }

    uint8_t codec = g_hda.primary_codec;
    uint32_t resp = 0;

    return hda_send_verb_best_available(codec, dac_nid,
                                        HDA_VERB_SET_CONVERTER_FORMAT,
                                        fmt,
                                        &resp);
}

static void hda_fill_square_wave_16le(uint8_t* buffer, size_t bytes) {
    if (!buffer || bytes < 4) return;

    int16_t* samples = (int16_t*)buffer;
    size_t sample_count = bytes / sizeof(int16_t);
    int16_t high = 12000;
    int16_t low  = -12000;
    size_t period_samples = 48; // ~1 kHz at 48 kHz

    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        size_t phase = (i / 2) % period_samples;
        int16_t value = (phase < (period_samples / 2)) ? high : low;
        samples[i]     = value;
        samples[i + 1] = value;
    }
}

bool hda_start_output_playback(void) {
    if (!g_hda.present || !g_hda.mmio_base) return false;
    if (!g_hda.codec_present) return false;
    if (!g_hda.out_dac_nid || !g_hda.out_pin_nid) return false;

    uint8_t out_streams = hda_output_stream_count();
    if (out_streams == 0) {
        return false;
    }

    uint8_t stream_index = 0xFF;
    for (uint8_t i = 0; i < out_streams; ++i) {
        size_t off = hda_stream_offset(i);
        uint32_t ctl = hda_mmio_read32(off + HDA_SD_CTL);
        if ((ctl & HDA_SDCTL_RUN) == 0) {
            stream_index = i;
            break;
        }
    }

    if (stream_index == 0xFF) {
        return false; // no free output stream
    }

    if (!hda_reset_stream(stream_index)) {
        return false;
    }

    void* bdl_page = pmm_alloc();
    if (!bdl_page) {
        return false;
    }
    uint64_t bdl_phys = (uint64_t)(uintptr_t)bdl_page;
    hda_bdl_entry_t* bdl = (hda_bdl_entry_t*)phys_to_virt(bdl_phys);
    memset(bdl, 0, PAGE_SIZE);

    void* buffer_page = pmm_alloc();
    if (!buffer_page) {
        pmm_free(bdl_page);
        return false;
    }

    uint64_t buffer_phys = (uint64_t)(uintptr_t)buffer_page;
    uint8_t* buffer_virt = (uint8_t*)phys_to_virt(buffer_phys);
    size_t buffer_bytes = PAGE_SIZE;

    memset(buffer_virt, 0, buffer_bytes);
    hda_fill_square_wave_16le(buffer_virt, buffer_bytes);

    bdl[0].address = buffer_phys;
    bdl[0].length  = (uint32_t)buffer_bytes;
    bdl[0].flags   = 1; // Interrupt on completion

    size_t off = hda_stream_offset(stream_index);

    // Program buffer descriptor list
    hda_mmio_write32(off + HDA_SD_BDPL, (uint32_t)(bdl_phys & 0xFFFFFFFFu));
    hda_mmio_write32(off + HDA_SD_BDPU, (uint32_t)(bdl_phys >> 32));

    // Program cyclic buffer length and last valid index (single entry)
    hda_mmio_write32(off + HDA_SD_CBL, (uint32_t)buffer_bytes);
    hda_mmio_write16(off + HDA_SD_LVI, 0);

    // 48kHz, 16-bit, 2-channel: channel count (2 - 1) | (16-bit << 4) | (48kHz << 8)
    uint16_t fmt = (uint16_t)((1u) | (1u << 4));
    hda_mmio_write16(off + HDA_SD_FMT, fmt);

    // Program stream tag and interrupt enables
    uint8_t stream_tag = (uint8_t)(stream_index + 1);
    uint32_t ctl = hda_mmio_read32(off + HDA_SD_CTL);
    ctl &= ~(HDA_SDCTL_STREAM_TAG_MASK | HDA_SDCTL_STRIPE_MASK | HDA_SDCTL_TRAFFIC_PRIO);
    ctl |= ((uint32_t)stream_tag << HDA_SDCTL_STREAM_TAG_SHIFT);
    ctl |= HDA_SDCTL_IOCE | HDA_SDCTL_FEIE | HDA_SDCTL_DEIE;
    hda_mmio_write32(off + HDA_SD_CTL, ctl);

    if (!hda_codec0_set_converter_format(g_hda.out_dac_nid, fmt)) {
        return false;
    }

    if (!hda_codec0_set_converter_stream_channel(g_hda.out_dac_nid, stream_tag, 0)) {
        return false;
    }

    // Enable interrupt for this stream and globally
    uint32_t intctl = hda_mmio_read32(HDA_REG_INTCTL);
    intctl |= (1u << stream_index) | HDA_INTCTL_GIE;
    hda_mmio_write32(HDA_REG_INTCTL, intctl);

    // Kick RUN bit
    ctl |= HDA_SDCTL_RUN;
    hda_mmio_write32(off + HDA_SD_CTL, ctl);

    g_hda.playback_stream_index = stream_index;
    g_hda.playback_stream_tag   = stream_tag;
    g_hda.playback_buffer_phys  = buffer_phys;
    g_hda.playback_bdl_phys     = bdl_phys;
    g_hda.playback_buffer_bytes = buffer_bytes;
    g_hda.playback_stream_ready = true;

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
uint8_t hda_get_irq_line(void) {
    return g_hda.irq_line;
}
