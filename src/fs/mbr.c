#include "fs/mbr.h"
#include "drivers/blockdev.h"
#include "lib/string.h"
#include "memory/heap.h"

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

    block_device_t* child = (block_device_t*)kmalloc(sizeof(block_device_t));
    if (!child) {
        return 0;
    }

    memset(child, 0, sizeof(block_device_t));

    const char* src = parent->name ? parent->name : "disk";
    size_t base_len = strlen(src);
    if (base_len > 28) {
        base_len = 28;
    }

    size_t name_len = base_len + 2; // room for "p" + digit
    char* pname = (char*)kmalloc(name_len + 1);
    if (!pname) {
        kfree(child);
        return 0;
    }

    memcpy(pname, src, base_len);
    pname[base_len] = 'p';
    pname[base_len + 1] = (char)('1' + chosen);
    pname[base_len + 2] = '\0';

    child->name = pname;
    child->unit = parent->unit;
    child->total_sectors = m.parts[chosen].lba_count;
    child->base_lba = (uint64_t)m.parts[chosen].lba_first + parent->base_lba;
    child->driver_data = parent->driver_data;
    child->read = parent->read;
    child->write = parent->write;
    child->next = 0;

    blockdev_register(child);
    return child;
}
