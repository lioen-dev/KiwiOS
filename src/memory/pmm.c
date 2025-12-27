#include "memory/pmm.h"
#include "limine.h"
#include "libc/string.h"
#include "memory/hhdm.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// The bitmap - each bit represents one page
static uint8_t* bitmap = NULL;
static size_t bitmap_size = 0; // size in bytes
static size_t total_pages = 0;
static size_t used_pages = 0;
static size_t search_cursor = 0; // Hint for next allocation

// Highest physical address we know about
static uint64_t highest_addr = 0;

// Helper: Set a bit in the bitmap (mark page as used)
static inline void bitmap_set(size_t index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

// Helper: Clear a bit in the bitmap (mark page as free)
static inline void bitmap_clear(size_t index) {
    bitmap[index / 8] &= ~(1 << (index % 8));
}

// Helper: Test if a bit is set
static inline bool bitmap_test(size_t index) {
    return bitmap[index / 8] & (1 << (index % 8));
}

// Try to allocate a contiguous run of pages within [start, end)
static void* allocate_run_from(size_t start, size_t end, size_t count) {
    size_t idx = start;
    while (idx + count <= end) {
        size_t run_length = 0;
        while (idx + run_length < end && run_length < count && !bitmap_test(idx + run_length)) {
            run_length++;
        }

        if (run_length == count) {
            for (size_t j = 0; j < count; j++) {
                bitmap_set(idx + j);
            }
            used_pages += count;
            search_cursor = (idx + count) % total_pages;
            return (void*)(idx * PAGE_SIZE);
        }

        // Skip past the used page that ended the run
        idx += run_length + 1;
    }
    return NULL;
}

void pmm_init(struct limine_memmap_response* memmap) {
    if (memmap == NULL) {
        return;
    }
    
    // First pass: Find the highest memory address
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        uint64_t top = entry->base + entry->length;
        if (top > highest_addr) {
            highest_addr = top;
        }
    }
    
    total_pages = highest_addr / PAGE_SIZE;
    bitmap_size = (total_pages + 7) / 8;
    
    // Second pass: Find a place to put our bitmap
    uint64_t bitmap_phys = 0;  // Physical address
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            bitmap_phys = entry->base;
            bitmap = hhdm_phys_to_virt(bitmap_phys);  // Convert to virtual!
            break;
        }
    }
    
    if (bitmap == NULL) {
        return;
    }
    
    // Initialize bitmap: mark everything as used first
    memset(bitmap, 0xFF, bitmap_size);
    used_pages = total_pages;
    
    // Third pass: Mark free regions
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t base = entry->base;
            uint64_t length = entry->length;
            
            for (uint64_t addr = base; addr < base + length; addr += PAGE_SIZE) {
                size_t page_index = addr / PAGE_SIZE;
                if (page_index < total_pages && bitmap_test(page_index)) {
                    bitmap_clear(page_index);
                    used_pages--;
                }
            }
        }
    }
    
    // Mark the bitmap itself as used
    size_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = 0; i < bitmap_pages; i++) {
        size_t page_index = (bitmap_phys + i * PAGE_SIZE) / PAGE_SIZE;
        if (page_index < total_pages && !bitmap_test(page_index)) {
            bitmap_set(page_index);
            used_pages++;
        }
    }

    search_cursor = 0;
}

void* pmm_alloc(void) {
    if (total_pages == 0) return NULL;

    // Find next free page, starting from the cursor to avoid full rescans
    for (size_t scanned = 0; scanned < total_pages; scanned++) {
        size_t idx = (search_cursor + scanned) % total_pages;
        if (!bitmap_test(idx)) {
            bitmap_set(idx);
            used_pages++;
            search_cursor = (idx + 1) % total_pages;
            return (void*)(idx * PAGE_SIZE);
        }
    }

    // No free pages
    return NULL;
}

void pmm_free(void* addr) {
    if (addr == NULL) return;
    
    size_t page_index = (uint64_t)addr / PAGE_SIZE;
    
    if (page_index >= total_pages) return; // Invalid address
    if (!bitmap_test(page_index)) return; // Already free
    
    bitmap_clear(page_index);
    used_pages--;
    if (page_index < search_cursor) {
        search_cursor = page_index;
    }
}

void* pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;
    if (count > total_pages) return NULL;
    if (count == 1) return pmm_alloc();

    // Try from cursor to the end, then wrap around
    void* result = allocate_run_from(search_cursor, total_pages, count);
    if (!result && search_cursor > 0) {
        result = allocate_run_from(0, search_cursor, count);
    }
    if (result) return result;

    // Couldn't find enough contiguous pages
    return NULL;
}

void pmm_free_pages(void* addr, size_t count) {
    if (addr == NULL || count == 0) return;
    
    size_t start_page = (uint64_t)addr / PAGE_SIZE;
    
    for (size_t i = 0; i < count; i++) {
        size_t page_index = start_page + i;
        if (page_index >= total_pages) break;
        
        if (bitmap_test(page_index)) {
            bitmap_clear(page_index);
            used_pages--;
            if (page_index < search_cursor) {
                search_cursor = page_index;
            }
        }
    }
}

void pmm_get_stats(size_t* total, size_t* used, size_t* free) {
    if (total) *total = total_pages;
    if (used) *used = used_pages;
    if (free) *free = total_pages - used_pages;
}