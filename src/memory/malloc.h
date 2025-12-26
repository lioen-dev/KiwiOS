// malloc.h - Simple userspace malloc/free implementation
#ifndef MALLOC_H
#define MALLOC_H

#include "kiwilib.h"

// Block header for allocated memory
typedef struct malloc_block {
    size_t size;                   // Size of usable memory (not including header)
    int is_free;                   // 1 if free, 0 if allocated
    struct malloc_block* next;     // Next block in list
} malloc_block_t;

#define BLOCK_HEADER_SIZE sizeof(malloc_block_t)
#define ALIGN_SIZE 16

static malloc_block_t* heap_head = 0;

// Align size to 16 bytes
static size_t align_up(size_t size) {
    return (size + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1);
}

// Find a free block that fits
static malloc_block_t* find_free_block(size_t size) {
    malloc_block_t* current = heap_head;
    
    while (current) {
        if (current->is_free && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    
    return 0;  // No suitable block found
}

// Expand heap by allocating more memory
static malloc_block_t* expand_heap(size_t size) {
    // Request memory from kernel
    void* current_brk = brk(0);
    size_t total_size = BLOCK_HEADER_SIZE + size;
    
    void* new_brk = brk((void*)((uint64_t)current_brk + total_size));
    if (new_brk == current_brk) {
        return 0;  // Failed to expand
    }
    
    // Initialize new block
    malloc_block_t* block = (malloc_block_t*)current_brk;
    block->size = size;
    block->is_free = 0;
    block->next = 0;
    
    // Add to list
    if (!heap_head) {
        heap_head = block;
    } else {
        malloc_block_t* current = heap_head;
        while (current->next) {
            current = current->next;
        }
        current->next = block;
    }
    
    return block;
}

// Allocate memory
void* malloc(size_t size) {
    if (size == 0) return 0;
    
    size = align_up(size);
    
    // Try to find existing free block
    malloc_block_t* block = find_free_block(size);
    
    if (block) {
        // Found a free block
        block->is_free = 0;
        return (void*)((uint8_t*)block + BLOCK_HEADER_SIZE);
    }
    
    // Need to expand heap
    block = expand_heap(size);
    if (!block) return 0;
    
    return (void*)((uint8_t*)block + BLOCK_HEADER_SIZE);
}

// Free memory
void free(void* ptr) {
    if (!ptr) return;
    
    // Get block header
    malloc_block_t* block = (malloc_block_t*)((uint8_t*)ptr - BLOCK_HEADER_SIZE);
    block->is_free = 1;
    
    // TODO: Coalesce adjacent free blocks
    // TODO: Return memory to kernel when possible
}

// Allocate zeroed memory
void* calloc(size_t num, size_t size) {
    size_t total = num * size;
    void* ptr = malloc(total);
    
    if (ptr) {
        memset(ptr, 0, total);
    }
    
    return ptr;
}

// Reallocate memory
void* realloc(void* ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) {
        free(ptr);
        return 0;
    }
    
    // Get old block
    malloc_block_t* block = (malloc_block_t*)((uint8_t*)ptr - BLOCK_HEADER_SIZE);
    
    // If new size fits, just return existing pointer
    if (new_size <= block->size) {
        return ptr;
    }
    
    // Allocate new block
    void* new_ptr = malloc(new_size);
    if (!new_ptr) return 0;
    
    // Copy old data
    memcpy(new_ptr, ptr, block->size);
    
    // Free old block
    free(ptr);
    
    return new_ptr;
}

#endif