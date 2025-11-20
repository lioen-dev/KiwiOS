#ifndef FS_MBR_H
#define FS_MBR_H

#include <stdint.h>
#include <stdbool.h>

#include "drivers/blockdev.h"

#pragma pack(push,1)
typedef struct {
    uint8_t status;      // 0x80 bootable, 0x00 non-bootable
    uint8_t chs_first[3];
    uint8_t type;        // partition type
    uint8_t chs_last[3];
    uint32_t lba_first;  // starting LBA
    uint32_t lba_count;  // number of sectors
} mbr_part_t;

typedef struct {
    uint8_t bootcode[440];
    uint32_t disk_sig;
    uint16_t reserved;
    mbr_part_t parts[4];
    uint16_t magic;      // 0xAA55
} mbr_t;
#pragma pack(pop)

// Find the first Linux partition (0x83) or just the first non-zero type, and return a child device.
// Returns NULL on failure.
block_device_t* mbr_open_first_partition(block_device_t* parent);

#endif // FS_MBR_H
