#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

#include <stddef.h>
#include <stdint.h>

// Initialize the simple round-robin scheduler and register the timer tick hook.
void scheduler_init(void);

// Create a kernel-mode task backed by its own stack. Returns 0 on success.
int scheduler_create_kernel_task(const char *name, void (*entry)(void *), void *arg, size_t stack_size);

// Called from the PIT interrupt handler. Returns the stack pointer that should
// be used when unwinding the interrupt frame (may be the original pointer).
uint64_t *scheduler_tick(uint64_t *interrupt_rsp);

// Expose a tiny bit of state for diagnostics.
size_t scheduler_task_count(void);

// Print a short task listing to the console.
void scheduler_dump_tasks(void);

#endif // CORE_SCHEDULER_H
