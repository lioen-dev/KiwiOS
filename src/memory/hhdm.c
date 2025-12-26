#include "memory/hhdm.h"

uint64_t g_hhdm_offset = 0;

void hhdm_set_offset(uint64_t offset) {
    g_hhdm_offset = offset;
}

uint64_t hhdm_get_offset(void) {
    return g_hhdm_offset;
}
