#ifndef MEMORY_HEAP_H
#define MEMORY_HEAP_H

#include <stdint.h>
#include <stddef.h>

// Initialize the kernel heap
void heap_init(void);

// Allocate memory
void* kmalloc(size_t size);

// Free memory
void kfree(void* ptr);

// Allocate and zero memory
void* kcalloc(size_t num, size_t size);

// Reallocate memory
void* krealloc(void* ptr, size_t new_size);

// Get heap statistics
void heap_get_stats(size_t* total_allocated, size_t* total_free, size_t* num_allocations);

#endif // MEMORY_HEAP_H
