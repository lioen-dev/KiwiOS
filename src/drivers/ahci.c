#include "drivers/ahci.h"
#include "drivers/pci.h"
#include "drivers/blockdev.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "memory/pmm.h"
#include "arch/x86/io.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---------- PCI class codes ---------- */
#define AHCI_PCI_CLASS    0x01
#define AHCI_PCI_SUBCLASS 0x06
#define AHCI_PCI_PROGIF   0x01

#define AHCI_MAX_SLOTS    32
#define MAX_AHCI_DEVICES  8

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif

/* Optional PWT/PCD flags (x86-64) for uncached MMIO */
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

/* ---------- Driver state ---------- */
static ahci_dev_t     g_devs[MAX_AHCI_DEVICES];
static block_device_t g_bdevs[MAX_AHCI_DEVICES];
static int            g_dev_count = 0;

/* ---------- Small helpers ---------- */

static inline void cpu_pause(void) { __asm__ volatile("pause"); }
static inline void mfence_full(void){ __sync_synchronize(); }

static inline void spin_delay(int iters) {
    for (int i = 0; i < iters; ++i) cpu_pause();
}

static void* map_mmio_uncached(uint64_t phys, size_t size) {
    /* Map ABAR into the higher half at phys+offset with UC (PWT|PCD). */
    page_table_t* kpt = vmm_get_kernel_page_table();
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base_va = (uint64_t)phys_to_virt(phys);
    for (size_t i = 0; i < pages; ++i) {
        uint64_t pa = phys + i * PAGE_SIZE;
        uint64_t va = base_va + i * PAGE_SIZE;
        vmm_map_page(kpt, va, pa, PAGE_PRESENT | PAGE_WRITE | PAGE_PWT | PAGE_PCD);
    }
    return (void*)base_va;
}

static void stop_port(volatile hba_port_t* p) {
    p->cmd &= ~PxCMD_ST;
    p->cmd &= ~PxCMD_FRE;
    for (int i = 0; i < 10000; ++i) {        /* bounded & short */
        if ((p->cmd & (PxCMD_FR | PxCMD_CR)) == 0) break;
        spin_delay(32);
    }
}

static void start_port(volatile hba_port_t* p) {
    p->cmd |= PxCMD_FRE;
    p->cmd |= PxCMD_ST;
}

/* Return (DET,IPM) from PxSSTS */
static inline uint8_t port_det(uint32_t ssts) { return (uint8_t)(ssts & 0xF); }
static inline uint8_t port_ipm(uint32_t ssts) { return (uint8_t)((ssts >> 8) & 0xF); }

static bool has_active_link(volatile hba_port_t* p) {
    uint32_t ssts = p->ssts;
    return (port_det(ssts) == 3) && (port_ipm(ssts) == 1);
}

static void short_comreset(volatile hba_port_t* p) {
    uint32_t sctl = p->sctl;
    p->sctl = (sctl & ~PxSCTL_DET_MASK) | PxSCTL_DET_INIT;
    spin_delay(2000); /* ~ a few ms */
    p->sctl = (sctl & ~PxSCTL_DET_MASK) | PxSCTL_DET_NONE;
    spin_delay(2000);
}

/* Allocate a command table lazily for a slot if not present */
static void ensure_cmd_table(hba_cmd_header_t* hdr) {
    if (hdr->ctba || hdr->ctbau) return;
    uint64_t ct_phys = (uint64_t)pmm_alloc_pages(1);
    memset(phys_to_virt(ct_phys), 0, PAGE_SIZE);
    hdr->ctba  = (uint32_t)(ct_phys & 0xFFFFFFFFu);
    hdr->ctbau = (uint32_t)(ct_phys >> 32);
    hdr->prdt_length = 0;
    hdr->prd_byte_count = 0;
}

/* CI slot search (non-NCQ) */
static int find_free_slot(volatile hba_port_t* p) {
    uint32_t ci = p->ci; /* non-NCQ path uses CI only */
    for (int i = 0; i < AHCI_MAX_SLOTS; ++i) {
        if ((ci & (1u << i)) == 0) return i;
    }
    return -1;
}

/* ---------- Low-level command ---------- */
static bool issue_rw(ahci_dev_t* dev, uint8_t cmd, uint64_t lba, uint32_t count,
                     void* user_buf, bool do_write)
{
    if (!count || !user_buf) return false;

    volatile hba_port_t* port = dev->port;

    int slot = find_free_slot(port);
    if (slot < 0) return false;

    /* Command list base */
    hba_cmd_header_t* headers =
        (hba_cmd_header_t*)phys_to_virt(((uint64_t)port->clbu << 32) | port->clb);
    hba_cmd_header_t* hdr = &headers[slot];

    /* Ensure a command table exists for this slot (lazy) */
    ensure_cmd_table(hdr);

    /* Resolve command table area for this slot */
    hba_cmd_tbl_t* tbl =
        (hba_cmd_tbl_t*)phys_to_virt(((uint64_t)hdr->ctbau << 32) | hdr->ctba);

    /* Clear dynamic state */
    memset(tbl, 0, sizeof(hba_cmd_tbl_t));
    hdr->atapi = 0;
    hdr->write = do_write ? 1 : 0;      /* 1 = host->device (WRITE) */
    hdr->prefetch = 0;
    hdr->reset = 0;
    hdr->bist = 0;
    hdr->clear = 1;
    hdr->pmp = 0;
    hdr->prd_byte_count = 0;
    hdr->cfl = 5; /* EXACTLY 5 dwords for Reg H2D FIS */

    /* Build CFIS (LBA48) */
    fis_reg_h2d_t* cfis = (fis_reg_h2d_t*)tbl->cfis;
    memset(cfis, 0, sizeof(*cfis));
    cfis->fis_type = FIS_TYPE_REG_H2D;
    cfis->c = 1;
    cfis->command = cmd;
    cfis->device = 1u << 6; /* LBA mode */
    cfis->lba0 = (uint8_t)(lba & 0xFF);
    cfis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    cfis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    cfis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    cfis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    cfis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    cfis->countl = (uint8_t)(count & 0xFF);
    cfis->counth = (uint8_t)((count >> 8) & 0xFF);

    /* ----------- DMA bounce buffer (physically contiguous) ----------- */
    size_t xfer_bytes = (size_t)count * SECTOR_SIZE;
    size_t pages = (xfer_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t dma_phys = (uint64_t)pmm_alloc_pages(pages);
    if (!dma_phys) return false;
    uint8_t* dma_virt = (uint8_t*)phys_to_virt(dma_phys);

    if (do_write) { memcpy(dma_virt, user_buf, xfer_bytes); }

    /* Fill PRDT: one entry per page (cap 128 entries) */
    uint32_t prdt_count = 0;
    size_t remaining = xfer_bytes;
    uint64_t page_pa = dma_phys;

    while (remaining > 0) {
        if (prdt_count >= 128) {
            pmm_free_pages((void*)dma_phys, pages);
            return false;
        }
        size_t chunk = remaining > PAGE_SIZE ? PAGE_SIZE : remaining;
        if (chunk > (4u * 1024u * 1024u)) chunk = 4u * 1024u * 1024u;

        hba_prdt_entry_t* e = &tbl->prdt[prdt_count++];
        e->dba  = (uint32_t)(page_pa & 0xFFFFFFFFu);
        e->dbau = (uint32_t)(page_pa >> 32);
        e->rsv0 = 0;
        e->dbc  = (uint32_t)(chunk - 1);
        e->i    = 1;

        remaining -= chunk;
        page_pa  += chunk;
    }

    hdr->prdt_length = prdt_count;

    /* Clear status before issuing */
    port->is   = 0xFFFFFFFFu;
    port->serr = 0xFFFFFFFFu;

    /* Wait not busy (BSY/DRQ clear) – bounded */
    for (int i = 0; i < 20000; ++i) {
        uint32_t tfd = port->tfd;
        if ((tfd & 0x88) == 0) break;
        spin_delay(16);
    }

    /* Memory fence before HBA DMA sees our descriptors */
    mfence_full();

    /* Kick the command */
    port->ci |= (1u << slot);

    /* Poll for completion (bounded) */
    bool ok = true;
    for (int i = 0; i < 500000; ++i) {
        if ((port->ci & (1u << slot)) == 0) break;            /* done */
        if (port->is & (1u << 30)) { ok = false; break; }     /* TFES */
        spin_delay(8);
    }
    if (port->is & (1u << 30)) ok = false;

    if (ok && !do_write) { memcpy(user_buf, dma_virt, xfer_bytes); }

    pmm_free_pages((void*)dma_phys, pages);
    return ok;
}

/* IDENTIFY for optional geometry */
static bool identify(ahci_dev_t* dev, uint16_t* id512) {
    return issue_rw(dev, 0xEC, 0, 1, id512, false); /* IDENTIFY DEVICE */
}

/* ---------- Per-port init ---------- */

static void init_port_minimal_hw(volatile hba_port_t* p) {
    /* Clear pending state and power on/spin */
    p->is   = 0xFFFFFFFFu;
    p->serr = 0xFFFFFFFFu;
    p->cmd |= PxCMD_POD | PxCMD_SUD;
}

/* Allocate CLB/FB and start FIS receive + command engine */
static bool bringup_port_runtime(volatile hba_port_t* p) {
    /* Stop to reprogram safely */
    stop_port(p);

    /* Command list & FIS buffers (1 page each) */
    uint64_t cl_phys = (uint64_t)pmm_alloc_pages(1);
    if (!cl_phys) return false;
    memset(phys_to_virt(cl_phys), 0, PAGE_SIZE);
    p->clb  = (uint32_t)(cl_phys & 0xFFFFFFFFu);
    p->clbu = (uint32_t)(cl_phys >> 32);

    uint64_t fb_phys = (uint64_t)pmm_alloc_pages(1);
    if (!fb_phys) return false;
    memset(phys_to_virt(fb_phys), 0, PAGE_SIZE);
    p->fb  = (uint32_t)(fb_phys & 0xFFFFFFFFu);
    p->fbu = (uint32_t)(fb_phys >> 32);

    /* Initialize headers (no CTBAs yet; allocated lazily per slot) */
    hba_cmd_header_t* hdr = (hba_cmd_header_t*)phys_to_virt(cl_phys);
    for (int s = 0; s < AHCI_MAX_SLOTS; ++s) {
        hdr[s].ctba  = 0;
        hdr[s].ctbau = 0;
        hdr[s].prdt_length = 0;
        hdr[s].prd_byte_count = 0;
        hdr[s].cfl = 5;
        hdr[s].atapi = 0;
        hdr[s].write = 0;
        hdr[s].prefetch = 0;
        hdr[s].reset = 0;
        hdr[s].bist = 0;
        hdr[s].clear = 1;
        hdr[s].pmp = 0;
    }

    start_port(p);

    /* Give hardware a brief moment to latch */
    spin_delay(512);
    return true;
}

/* ---------- Block device glue ---------- */

static bool bdev_read(block_device_t* b, uint64_t lba, uint32_t count, void* buf) {
    if (!b || !buf || !count) return false;
    ahci_dev_t* d = (ahci_dev_t*)b->driver_data;
    return issue_rw(d, 0x25, lba, count, buf, false); /* READ DMA EXT */
}

static bool bdev_write(block_device_t* b, uint64_t lba, uint32_t count, const void* buf) {
    if (!b || !buf || !count) return false;
    ahci_dev_t* d = (ahci_dev_t*)b->driver_data;
    return issue_rw(d, 0x35, lba, count, (void*)buf, true); /* WRITE DMA EXT */
}

/* ---------- Entry point ---------- */

int ahci_init(void) {
    pci_device_t devs[64];
    int n = pci_enum_devices(devs, 64);
    int found = 0;

    for (int i = 0; i < n && g_dev_count < MAX_AHCI_DEVICES; ++i) {
        pci_device_t* pdev = &devs[i];
        if (pdev->class_code != AHCI_PCI_CLASS ||
            pdev->subclass   != AHCI_PCI_SUBCLASS ||
            pdev->prog_if    != AHCI_PCI_PROGIF)
            continue;

        /* Enable MMIO + bus mastering */
        pci_enable_mmio_and_bus_mastering(pdev->bus, pdev->slot, pdev->func);

        /* BAR5 (index 5) is ABAR */
        bool is_io = false;
        uint8_t size_bits = 0;
        uint32_t bar5 = pci_read_bar(pdev->bus, pdev->slot, pdev->func, 5, &is_io, &size_bits);
        if (is_io || bar5 == 0 || bar5 == 0xFFFFFFFFu) continue;

        uint64_t abar_phys = (uint64_t)(bar5 & ~0xFu);
        hba_mem_t* abar = (hba_mem_t*)map_mmio_uncached(abar_phys, 0x2000);

        /* BIOS/OS handoff if supported (best-effort, bounded) */
        if (abar->cap2 & 1u) {
            abar->bohc |= (1u << 1);                 /* OOS: OS owns */
            for (int t = 0; t < 50000; ++t) {        /* short bound */
                if ((abar->bohc & 1u) == 0) break;   /* BOS clear -> BIOS not busy */
                spin_delay(16);
            }
        }

        /* Enable AHCI */
        abar->ghc |= AHCI_GHC_AE;

        /* Scan implemented ports */
        uint32_t pi = abar->pi;
        for (int port = 0; port < 32 && pi; ++port, pi >>= 1) {
            if ((pi & 1u) == 0) continue;

            volatile hba_port_t* hp = &abar->ports[port];

            /* Minimal HW prep & fast link check */
            init_port_minimal_hw(hp);

            uint32_t ssts0 = hp->ssts;
            if (port_det(ssts0) == 0) {
                /* No device electrically present – skip without COMRESET */
                continue;
            }
            if (!has_active_link(hp)) {
                /* Try one short COMRESET for DET==1 link-down cases */
                short_comreset(hp);
                if (!has_active_link(hp)) continue;
            }

            /* Program CLB/FB and start engine; signature will be latched */
            if (!bringup_port_runtime(hp)) continue;

            /* Only register plain SATA disks here */
            if (hp->sig != SATA_SIG_ATA) continue;

            /* Build device wrapper */
            ahci_dev_t* ad = &g_devs[g_dev_count];
            ad->abar = abar;
            ad->port = hp;
            ad->port_num = (uint8_t)port;

            /* Optional: IDENTIFY for capacity */
            uint16_t* id = (uint16_t*)kmalloc(512);
            uint64_t total_sectors = 0;
            if (id && identify(ad, id)) {
                total_sectors = ((uint64_t)id[100]) |
                                ((uint64_t)id[101] << 16) |
                                ((uint64_t)id[102] << 32) |
                                ((uint64_t)id[103] << 48);
            }
            if (id) kfree(id);

            block_device_t* b = &g_bdevs[g_dev_count];
            char namebuf[8] = "ahci0";
            namebuf[4] = (char)('0' + g_dev_count);
            b->name = (char*)kmalloc(6);
            memcpy((char*)b->name, namebuf, 6);
            b->unit = g_dev_count;
            b->driver_data = ad;
            b->read  = bdev_read;
            b->write = bdev_write;
            b->base_lba = 0;
            b->total_sectors = total_sectors;

            blockdev_register(b);
            g_dev_count++;
            found++;
        }
    }

    return found;
}
