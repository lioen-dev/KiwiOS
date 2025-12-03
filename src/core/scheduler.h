#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

void scheduler_init(void);

// Force a reschedule using the provided interrupt stack frame. The frame layout
// matches interrupt_context_t and syscall_frame_t. Returns true if a different
// process was selected.
bool scheduler_reschedule(uint64_t* interrupt_rsp);

#endif // CORE_SCHEDULER_H
