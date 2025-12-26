#include "drivers/blockdev.h"

static block_device_t* g_head = 0;
static block_device_t* g_root = 0;

void blockdev_init(void) {
    g_head = 0;
    g_root = 0;
}

block_device_t* blockdev_register(block_device_t* dev) {
    if (!dev) return 0;
    dev->next = g_head;
    g_head = dev;
    if (!g_root) g_root = dev;
    return dev;
}

block_device_t* blockdev_get_root(void) {
    return g_root;
}
void blockdev_set_root(block_device_t* dev) {
    g_root = dev;
}

block_device_t* blockdev_first(void) {
    return g_head;
}
block_device_t* blockdev_next(block_device_t* it) {
    return it ? it->next : 0;
}