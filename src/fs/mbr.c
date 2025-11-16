#include "fs/mbr.h"
#include "drivers/blockdev.h"
#include "lib/string.h"

static block_device_t g_part;

block_device_t* mbr_open_first_partition(block_device_t* parent) {
    if (!parent) return 0;
    uint8_t buf[SECTOR_SIZE];
    if (!block_read(parent, 0, 1, buf)) return 0;

    mbr_t m;
    memcpy(&m, buf, sizeof(m));
    if (m.magic != 0xAA55) return 0;

    int chosen = -1;
    for (int i=0; i<4; ++i) {
        if (m.parts[i].type == 0) continue; 
        if (m.parts[i].type == 0xEE) continue;
        if (chosen == -1) chosen = i;
        if (m.parts[i].type == 0x83) { chosen = i; break; } // prefer Linux
    }
    if (chosen < 0) return 0;

        // Build a child name based on parent, e.g., "<parent>pN"
    static char pname[32];
    char* pn = pname;
    const char* src = parent->name ? parent->name : "disk";
    while (*src && (pn - pname) < (int)sizeof(pname) - 3) *pn++ = *src++;
    *pn++ = 'p'; *pn++ = (char)('1' + chosen); *pn = 0;
    g_part.name = pname;
    g_part.unit = parent->unit;
    g_part.total_sectors = m.parts[chosen].lba_count;
    g_part.base_lba = (uint64_t)m.parts[chosen].lba_first + parent->base_lba;
    g_part.driver_data = parent->driver_data;
    g_part.read = parent->read;
    g_part.write = parent->write;
    g_part.next = 0;
    blockdev_register(&g_part);
    return &g_part;
}