#ifndef DRIVERS_BLOCKDEV_H
#define DRIVERS_BLOCKDEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SECTOR_SIZE 512

typedef struct block_device block_device_t;

typedef bool (*blk_read_fn)(block_device_t* dev, uint64_t lba, uint32_t count, void* buf);
typedef bool (*blk_write_fn)(block_device_t* dev, uint64_t lba, uint32_t count, const void* buf);

struct block_device {
    const char* name;
    uint64_t    total_sectors;     // size in 512B sectors (may be 0 if unknown)
    uint64_t    base_lba;          // LBA offset for partitions (0 for whole disk)
    void*       driver_data;       // driver-private pointer
    blk_read_fn  read;
    blk_write_fn write;            // may be NULL for read-only
    int         unit;              // driver-defined unit index (e.g., 0 for primary master)
    block_device_t* next;
};

// Device registry API
void blockdev_init(void);
block_device_t* blockdev_register(block_device_t* dev);
block_device_t* blockdev_get_root(void);
void blockdev_set_root(block_device_t* dev);
block_device_t* blockdev_first(void);
block_device_t* blockdev_next(block_device_t* it);

// Helpers
static inline bool block_read(block_device_t* dev, uint64_t lba, uint32_t count, void* buf) {
    return dev && dev->read && dev->read(dev, dev->base_lba + lba, count, buf);
}

static inline bool block_write(block_device_t* dev, uint64_t lba, uint32_t count, const void* buf) {
    return dev && dev->write && dev->write(dev, dev->base_lba + lba, count, buf);
}

#endif // DRIVERS_BLOCKDEV_H
