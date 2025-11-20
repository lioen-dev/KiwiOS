#ifndef FS_GPT_H
#define FS_GPT_H

#include <stdint.h>
#include <stdbool.h>

#include "drivers/blockdev.h"

#define GPT_SIG 0x5452415020494645ULL /* "EFI PART" */

typedef struct __attribute__((packed)) {
    uint64_t signature;             // "EFI PART"
    uint32_t revision;              // 0x00010000
    uint32_t header_size;           // usually 92
    uint32_t header_crc32;
    uint32_t reserved0;
    uint64_t current_lba;           // LBA of this header (usually 1)
    uint64_t backup_lba;            // LBA of backup header (usually last LBA)
    uint64_t first_usable_lba;      // after primary entries
    uint64_t last_usable_lba;       // before backup entries
    uint8_t  disk_guid[16];
    uint64_t entries_lba;           // start of partition entries (usually 2)
    uint32_t num_entries;           // usually 128
    uint32_t entry_size;            // usually 128
    uint32_t entries_crc32;
    uint8_t  pad[512-92];           // pad to sector (we always read/write whole sector)
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name_utf16[36];        // UTF-16LE name (optional)
} gpt_entry_t;

bool gpt_read(block_device_t* disk, gpt_header_t* hdr, gpt_entry_t** entries);
bool gpt_write(block_device_t* disk, const gpt_header_t* hdr, const gpt_entry_t* entries);

// Make/repair a protective MBR covering whole disk (type 0xEE)
bool gpt_write_protective_mbr(block_device_t* disk);

#endif // FS_GPT_H
