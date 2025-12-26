#ifndef MEMORY_PMM_H
#define MEMORY_PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "limine.h"

// Page size is 4KB (4096 bytes)
#define PAGE_SIZE 4096

// Initialize the physical memory manager
void pmm_init(struct limine_memmap_response* memmap);

// Allocate a single physical page (4KB)
// Returns physical address of the page, or 0 if no memory available
void* pmm_alloc(void);

// Free a physical page
// addr must be the address returned by pmm_alloc()
void pmm_free(void* addr);

// Allocate multiple contiguous pages
// Returns physical address of the first page, or 0 if can't find contiguous space
void* pmm_alloc_pages(size_t count);

// Free multiple contiguous pages
void pmm_free_pages(void* addr, size_t count);

// Get statistics about memory usage
void pmm_get_stats(size_t* total_pages, size_t* used_pages, size_t* free_pages);

#endif // MEMORY_PMM_H
