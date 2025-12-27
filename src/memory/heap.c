#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "libc/string.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>


// Block header for each allocation
typedef struct heap_block {
    size_t size;              // Size of the block (not including header)
    bool is_free;             // Is this block free?
    struct heap_block* next;  // Next block in the list
    struct heap_block* prev;  // Previous block in the list
} heap_block_t;

#define BLOCK_HEADER_SIZE sizeof(heap_block_t)
#define MIN_ALLOC_SIZE 16  // Minimum allocation size
#define HEAP_MAGIC 0xDEADBEEF  // For debugging

// Head of the free list
static heap_block_t* heap_start = NULL;
static size_t total_heap_size = 0;
static size_t total_allocated = 0;
static size_t num_allocations = 0;

// Align size to 16 bytes
static inline size_t align_size(size_t size) {
    return (size + 15) & ~15;
}

// Expand the heap by allocating more pages
static heap_block_t* expand_heap(size_t size) {
    // Calculate how many pages we need
    size_t total_needed = size + BLOCK_HEADER_SIZE;
    size_t pages_needed = (total_needed + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Allocate pages
    void* new_mem = pmm_alloc_pages(pages_needed);
    if (!new_mem) return NULL;
    
    // Convert to virtual address
    heap_block_t* block = (heap_block_t*)phys_to_virt((uint64_t)new_mem);
    
    // Initialize the block
    block->size = (pages_needed * PAGE_SIZE) - BLOCK_HEADER_SIZE;
    block->is_free = true;
    block->next = NULL;
    block->prev = NULL;
    
    total_heap_size += pages_needed * PAGE_SIZE;
    
    return block;
}

// Split a block if it's too large
static void split_block(heap_block_t* block, size_t size) {
    // Only split if there's enough room for another block
    if (block->size >= size + BLOCK_HEADER_SIZE + MIN_ALLOC_SIZE) {
        // Create new block after this one
        heap_block_t* new_block = (heap_block_t*)((uint8_t*)block + BLOCK_HEADER_SIZE + size);
        new_block->size = block->size - size - BLOCK_HEADER_SIZE;
        new_block->is_free = true;
        new_block->next = block->next;
        new_block->prev = block;
        
        if (block->next) {
            block->next->prev = new_block;
        }
        block->next = new_block;
        block->size = size;
    }
}

// Merge adjacent free blocks
static void merge_free_blocks(heap_block_t* block) {
    // Merge with next block if it's free
    if (block->next && block->next->is_free) {
        block->size += BLOCK_HEADER_SIZE + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }
    
    // Merge with previous block if it's free
    if (block->prev && block->prev->is_free) {
        block->prev->size += BLOCK_HEADER_SIZE + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
    }
}

void heap_init(void) {
    // Start with 4 pages (16KB)
    heap_start = expand_heap(PAGE_SIZE * 4);
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    // Align size
    size = align_size(size);
    
    // Find a free block that's big enough
    heap_block_t* current = heap_start;
    while (current) {
        if (current->is_free && current->size >= size) {
            // Found a suitable block
            split_block(current, size);
            current->is_free = false;
            total_allocated += current->size;
            num_allocations++;
            
            // Return pointer to memory after the header
            return (void*)((uint8_t*)current + BLOCK_HEADER_SIZE);
        }
        current = current->next;
    }
    
    // No suitable block found, expand heap
    heap_block_t* new_block = expand_heap(size);
    if (!new_block) return NULL;
    
    // Add to end of list
    if (heap_start == NULL) {
        heap_start = new_block;
    } else {
        heap_block_t* last = heap_start;
        while (last->next) last = last->next;
        last->next = new_block;
        new_block->prev = last;
    }
    
    split_block(new_block, size);
    new_block->is_free = false;
    total_allocated += new_block->size;
    num_allocations++;
    
    return (void*)((uint8_t*)new_block + BLOCK_HEADER_SIZE);
}

void kfree(void* ptr) {
    if (!ptr) return;
    
    // Get block header
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - BLOCK_HEADER_SIZE);
    
    if (block->is_free) {
        // Double free - ignore
        return;
    }
    
    block->is_free = true;
    total_allocated -= block->size;
    num_allocations--;
    
    // Merge with adjacent free blocks
    merge_free_blocks(block);
}

void* kcalloc(size_t num, size_t size) {
    if (num != 0 && size > SIZE_MAX / num) {
        return NULL;
    }

    size_t total = num * size;
    void* ptr = kmalloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    // Get old block
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - BLOCK_HEADER_SIZE);
    size_t old_size = block->size;
    
    // If new size fits in current block, just return it
    if (align_size(new_size) <= old_size) {
        return ptr;
    }
    
    // Allocate new block
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    
    // Copy old data
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    
    // Free old block
    kfree(ptr);
    
    return new_ptr;
}

void heap_get_stats(size_t* total_alloc, size_t* total_fr, size_t* num_alloc) {
    if (total_alloc) *total_alloc = total_allocated;
    if (total_fr) *total_fr = total_heap_size - total_allocated;
    if (num_alloc) *num_alloc = num_allocations;
}