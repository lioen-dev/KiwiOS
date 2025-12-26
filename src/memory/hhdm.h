#ifndef MEMORY_HHDM_H
#define MEMORY_HHDM_H

#include <stdint.h>

// Global higher-half direct map offset provided by Limine.
extern uint64_t g_hhdm_offset;

// Set the global HHDM offset once Limine provides it.
void hhdm_set_offset(uint64_t offset);

// Retrieve the currently configured HHDM offset.
uint64_t hhdm_get_offset(void);

// Convert a physical address into its higher-half direct map virtual address.
static inline void* hhdm_phys_to_virt(uint64_t phys) {
    return (void*)(phys + g_hhdm_offset);
}

// Convert a higher-half direct map virtual address back to its physical address.
static inline uint64_t hhdm_virt_to_phys(const void* virt) {
    return (uint64_t)virt - g_hhdm_offset;
}

#endif // MEMORY_HHDM_H
