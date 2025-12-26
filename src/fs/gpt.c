#include "fs/gpt.h"
#include "drivers/blockdev.h"
#include "fs/mbr.h"
#include "memory/heap.h"
#include "lib/string.h"
#include <stdint.h>
#include <stddef.h>

// ---------------- CRC32 ----------------
static uint32_t crc32_table[256];
static void crc32_init(void) {
    static int inited = 0; if (inited) return; inited = 1;
    for (uint32_t i=0; i<256; ++i) {
        uint32_t c = i;
        for (int j=0; j<8; ++j) c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
}
static uint32_t crc32_calc(const void* data, size_t len) {
    crc32_init();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i=0;i<len;i++) c = crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFU;
}

// ---------------- Helpers ----------------
static bool rsec(block_device_t* d, uint64_t lba, uint32_t n, void* b) { return block_read(d, lba, n, b); }
static bool wsec(block_device_t* d, uint64_t lba, uint32_t n, const void* b) { return block_write(d, lba, n, b); }

// ---------------- GPT read ----------------
bool gpt_read(block_device_t* disk, gpt_header_t* hdr, gpt_entry_t** entries_out) {
    if (!disk || !hdr || !entries_out) return false;

    // Read primary header at LBA 1
    gpt_header_t tmp;
    if (!rsec(disk, 1, 1, &tmp)) return false;
    if (tmp.signature != GPT_SIG) return false;

    // Verify header CRC
    uint32_t old_crc = tmp.header_crc32;
    tmp.header_crc32 = 0;
    uint32_t calc = crc32_calc(&tmp, tmp.header_size);
    if (calc != old_crc) return false;

    // Read entries array
    size_t entries_bytes = (size_t)tmp.num_entries * tmp.entry_size;
    size_t entries_sectors = (entries_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;

    gpt_entry_t* entries = (gpt_entry_t*)kmalloc(entries_sectors * SECTOR_SIZE);
    if (!entries) return false;
    if (!rsec(disk, tmp.entries_lba, (uint32_t)entries_sectors, entries)) { kfree(entries); return false; }

    // Verify entries CRC
    if (crc32_calc(entries, entries_bytes) != tmp.entries_crc32) { kfree(entries); return false; }

    *hdr = tmp;
    *entries_out = entries;
    return true;
}

// ---------------- Protective MBR ----------------
bool gpt_write_protective_mbr(block_device_t* disk) {
    if (!disk) return false;
    uint8_t sec[SECTOR_SIZE]; memset(sec, 0, sizeof(sec));

    mbr_t* m = (mbr_t*)sec;
    m->magic = 0xAA55;

    // Fill a single 0xEE partition spanning the disk
    uint64_t total = disk->total_sectors ? disk->total_sectors : 0xFFFFFFFFULL;
    uint32_t lba_first = 1; // GPT header lives at 1
    uint64_t span = (total > 1) ? (total - 1) : 0;
    if (span > 0xFFFFFFFFULL) span = 0xFFFFFFFFULL;

    m->parts[0].status = 0x00;
    m->parts[0].type = 0xEE;
    m->parts[0].lba_first = lba_first;
    m->parts[0].lba_count = (uint32_t)span;

    return wsec(disk, 0, 1, sec);
}

// ---------------- GPT write (primary + backup) ----------------
bool gpt_write(block_device_t* disk, const gpt_header_t* hdr_in, const gpt_entry_t* entries_in) {
    if (!disk || !hdr_in || !entries_in) return false;

    // Copy and recalc primary header CRC + entries CRC
    gpt_header_t hdr = *hdr_in;

    size_t entries_bytes = (size_t)hdr.num_entries * hdr.entry_size;
    size_t entries_sectors = (entries_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;

    // Write primary entries
    if (!wsec(disk, hdr.entries_lba, (uint32_t)entries_sectors, entries_in)) return false;

    // entries CRC
    gpt_header_t prim = hdr;
    prim.entries_crc32 = crc32_calc(entries_in, entries_bytes);

    // header CRC
    prim.header_crc32 = 0;
    prim.header_crc32 = crc32_calc(&prim, prim.header_size);

    // Write primary header
    if (!wsec(disk, prim.current_lba, 1, &prim)) return false;

    // Compute backup locations
    uint64_t last_lba = disk->total_sectors ? (disk->total_sectors - 1) : hdr.backup_lba;
    uint64_t backup_entries_lba = last_lba - entries_sectors; // array just before backup header

    // Build backup header (swap current/backup + entries LBA)
    gpt_header_t back = prim;
    back.current_lba = last_lba;
    back.backup_lba  = prim.current_lba;
    back.entries_lba = backup_entries_lba;

    back.header_crc32 = 0;
    back.header_crc32 = crc32_calc(&back, back.header_size);

    // Write backup entries + header
    if (!wsec(disk, backup_entries_lba, (uint32_t)entries_sectors, entries_in)) return false;
    if (!wsec(disk, last_lba, 1, &back)) return false;

    // Make sure protective MBR exists
    if (!gpt_write_protective_mbr(disk)) return false;

    return true;
}
