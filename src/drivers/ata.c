#include "drivers/ata.h"
#include "arch/x86/io.h"
#include "drivers/blockdev.h"
#include <stdint.h>
#include <stdbool.h>

// Use the existing port I/O functions provided by your kernel.
extern void outb(uint16_t port, uint8_t val);
extern uint8_t inb(uint16_t port);
extern void outw(uint16_t port, uint16_t val);
extern uint16_t inw(uint16_t port);
extern void outl(uint16_t port, uint32_t val);
extern uint32_t inl(uint16_t port);

static inline void io_wait_400ns(uint16_t alt_status_port) {
    (void)inb(alt_status_port);
    (void)inb(alt_status_port);
    (void)inb(alt_status_port);
    (void)inb(alt_status_port);
}

// I/O port bases
typedef struct {
    uint16_t io;   // data/command base (e.g., 0x1F0 or 0x170)
    uint16_t ctrl; // control/alt-status (e.g., 0x3F6 or 0x376)
    uint8_t  slave;// 0=master, 1=slave
} ata_chan_t;

// Primary Master only by default (works in QEMU/VMware IDE mode)
static ata_chan_t g_chans[2] = {
    {0x1F0, 0x3F6, 0}, // primary master
    {0x170, 0x376, 0}, // secondary master
};
static int g_num = 0;

typedef struct {
    ata_chan_t chan;
    uint64_t   total_sectors;
} ata_dev_t;

static ata_dev_t g_devs[2];
static block_device_t g_blkdevs[2];

// ATA status bits
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DSC  0x10
#define ATA_SR_DRQ  0x08
#define ATA_SR_CORR 0x04
#define ATA_SR_IDX  0x02
#define ATA_SR_ERR  0x01

// ATA commands
#define ATA_CMD_READ_SECTORS   0x20
#define ATA_CMD_READ_SECTORS_EXT 0x24
#define ATA_CMD_WRITE_SECTORS  0x30
#define ATA_CMD_WRITE_SECTORS_EXT 0x34
#define ATA_CMD_IDENTIFY       0xEC

static bool ata_wait_busy_clear(ata_chan_t* c) {
    for (int i=0; i<100000; ++i) {
        uint8_t st = inb(c->io + 7);
        if (!(st & ATA_SR_BSY)) return true;
    }
    return false;
}
static bool ata_wait_drq(ata_chan_t* c) {
    for (int i=0; i<100000; ++i) {
        uint8_t st = inb(c->io + 7);
        if (st & ATA_SR_ERR) return false;
        if (st & ATA_SR_DRQ) return true;
    }
    return false;
}

// 28-bit LBA read/write
static bool ata_lba28_rw(ata_chan_t* c, uint64_t lba, uint32_t count, void* buf, bool is_write) {
    if (count == 0) return true;
    if (lba > 0x0FFFFFFF) return false; // beyond 28-bit range

    uint8_t* p = (uint8_t*)buf;
    for (uint32_t i=0; i<count; ++i) {
        if (!ata_wait_busy_clear(c)) return false;

        // Select drive (0xE0 for master) + LBA bits 24-27
        outb(c->io + 6, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));

        outb(c->io + 2, 1);                    // sector count = 1
        outb(c->io + 3, (uint8_t)(lba & 0xFF));
        outb(c->io + 4, (uint8_t)((lba >> 8) & 0xFF));
        outb(c->io + 5, (uint8_t)((lba >> 16) & 0xFF));
        outb(c->io + 7, is_write ? ATA_CMD_WRITE_SECTORS : ATA_CMD_READ_SECTORS);

        if (!ata_wait_drq(c)) return false;

        if (!is_write) {
            // Read 256 words
            uint16_t* pw = (uint16_t*)p;
            for (int w=0; w<256; ++w) pw[w] = inw(c->io + 0);
            io_wait_400ns(c->ctrl);
        } else {
            // Write 256 words
            const uint16_t* pw = (const uint16_t*)p;
            for (int w=0; w<256; ++w) outw(c->io + 0, pw[w]);
            io_wait_400ns(c->ctrl);
        }

        p += SECTOR_SIZE;
        lba++;
    }
    return true;
}

static bool ata_read(block_device_t* bdev, uint64_t lba, uint32_t count, void* buf) {
    ata_dev_t* d = (ata_dev_t*)bdev->driver_data;
    if (!d) return false;
    // For simplicity, use 28-bit path if LBA fits
    if ((lba + count) <= 0x10000000ULL) {
        return ata_lba28_rw(&d->chan, lba, count, buf, false);
    }
    // (Optional) could implement 48-bit here; for now, fail if out of range
    return false;
}

static bool ata_write(block_device_t* bdev, uint64_t lba, uint32_t count, const void* buf) {
    ata_dev_t* d = (ata_dev_t*)bdev->driver_data;
    if (!d) return false;
    if ((lba + count) <= 0x10000000ULL) {
        // Cast away const for the convenience function
        return ata_lba28_rw(&d->chan, lba, count, (void*)buf, true);
    }
    return false;
}

static bool ata_identify(ata_chan_t* c, uint16_t* out_id) {
    // Select drive
    outb(c->io + 6, c->slave ? 0xF0 : 0xE0);
    io_wait_400ns(c->ctrl);

    outb(c->io + 2, 0);
    outb(c->io + 3, 0);
    outb(c->io + 4, 0);
    outb(c->io + 5, 0);
    outb(c->io + 7, ATA_CMD_IDENTIFY);

    uint8_t st = inb(c->io + 7);
    if (st == 0) return false; // no device

    if (!ata_wait_busy_clear(c)) return false;
    if (!ata_wait_drq(c)) return false;

    for (int i=0; i<256; ++i) out_id[i] = inw(c->io + 0);
    io_wait_400ns(c->ctrl);
    return true;
}

int ata_init(void) {
    g_num = 0;
    for (int i=0; i<2; ++i) {
        uint16_t id[256];
        if (!ata_identify(&g_chans[i], id)) continue;

        // Word 60-61: total number of 28-bit-addressable LBA sectors
        uint32_t lba28 = ((uint32_t)id[61] << 16) | id[60];
        g_devs[g_num].chan = g_chans[i];
        g_devs[g_num].total_sectors = lba28 ? lba28 : 0;

        g_blkdevs[g_num].name = (i==0) ? "ata0" : "ata1";
        g_blkdevs[g_num].unit = i;
        g_blkdevs[g_num].total_sectors = g_devs[g_num].total_sectors;
        g_blkdevs[g_num].base_lba = 0;
        g_blkdevs[g_num].driver_data = &g_devs[g_num];
        g_blkdevs[g_num].read  = ata_read;
        g_blkdevs[g_num].write = ata_write;

        blockdev_register(&g_blkdevs[g_num]);
        g_num++;
    }
    return g_num;
}

block_device_t* ata_get_device(int index) {
    if (index < 0 || index >= g_num) return 0;
    return &g_blkdevs[index];
}