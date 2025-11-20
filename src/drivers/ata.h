#ifndef DRIVERS_ATA_H
#define DRIVERS_ATA_H

#include <stdint.h>
#include <stdbool.h>

#include "drivers/blockdev.h"

// Initialize legacy ATA PIO (primary/secondary). Registers the primary-master as a block device if found.
// Returns number of devices registered.
int ata_init(void);

// Expose discovered devices (0..N-1)
block_device_t* ata_get_device(int index);

#endif // DRIVERS_ATA_H
