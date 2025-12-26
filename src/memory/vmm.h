#ifndef MEMORY_VMM_H
#define MEMORY_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "memory/hhdm.h"

// Page size is 4KB
#define PAGE_SIZE 4096

// Page table entry flags
#define PAGE_PRESENT (1 << 0)
#define PAGE_WRITE (1 << 1)
#define PAGE_USER (1 << 2)

// Virtual address manipulation
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~0xFFFULL)
#define PAGE_ALIGN_UP(addr) (((addr) + 0xFFF) & ~0xFFFULL)

// Page table structure
typedef struct {
    uint64_t* pml4_phys;
    uint64_t* pml4_virt;
} page_table_t;

// Initialize VMM
void vmm_init(void);

// Create a new page table
page_table_t* vmm_create_page_table(void);

// Switch to a different page table
void vmm_switch_page_table(page_table_t* pt);

// Map a virtual address to a physical address
bool vmm_map_page(page_table_t* pt, uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a virtual address
void vmm_unmap_page(page_table_t* pt, uint64_t virt);

// Get the physical address for a virtual address
uint64_t vmm_get_physical(page_table_t* pt, uint64_t virt);

// Get the kernel page table
page_table_t* vmm_get_kernel_page_table(void);

// Helper: Convert physical to virtual address
static inline void* phys_to_virt(uint64_t phys) {
    return hhdm_phys_to_virt(phys);
}

// Helper: Convert virtual to physical address
static inline uint64_t virt_to_phys(void* virt) {
    return hhdm_virt_to_phys(virt);
}

#endif // MEMORY_VMM_H
