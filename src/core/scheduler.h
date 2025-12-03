#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

struct process;

void scheduler_init(void);

// Force a reschedule using the provided interrupt stack frame. The frame layout
// matches interrupt_context_t and syscall_frame_t. Returns true if a different
// process was selected.
bool scheduler_reschedule(uint64_t* interrupt_rsp);

// Put the current process to sleep until the specified tick count. Returns true
// if a context switch occurred (the caller will resume later), false if the
// sleep could not be scheduled.
bool scheduler_sleep_until(uint64_t* interrupt_rsp, uint64_t target_tick);

// Remove a process from the sleep queue (used during teardown).
void scheduler_cancel_sleep(struct process* proc);

#endif // CORE_SCHEDULER_H
