#include "core/scheduler.h"
#include "arch/x86/tss.h"
#include "core/process.h"
#include "drivers/timer.h"
#include "memory/vmm.h"

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

static volatile bool in_scheduler = false;
static process_t* sleep_queue_head = NULL;

static inline interrupt_context_t* frame_to_ctx(uint64_t* interrupt_rsp) {
    return (interrupt_context_t*)interrupt_rsp;
}

static void sleep_queue_remove(process_t* proc) {
    if (!proc) return;

    process_t* prev = NULL;
    process_t* iter = sleep_queue_head;

    while (iter) {
        if (iter == proc) {
            if (prev) {
                prev->sleep_next = iter->sleep_next;
            } else {
                sleep_queue_head = iter->sleep_next;
            }
            proc->sleep_next = NULL;
            return;
        }
        prev = iter;
        iter = iter->sleep_next;
    }
}

static void sleep_queue_insert(process_t* proc) {
    if (!proc) return;

    sleep_queue_remove(proc);

    if (!sleep_queue_head || proc->sleep_until < sleep_queue_head->sleep_until) {
        proc->sleep_next = sleep_queue_head;
        sleep_queue_head = proc;
        return;
    }

    process_t* iter = sleep_queue_head;
    while (iter->sleep_next && iter->sleep_next->sleep_until <= proc->sleep_until) {
        iter = iter->sleep_next;
    }

    proc->sleep_next = iter->sleep_next;
    iter->sleep_next = proc;
}

static void wake_sleeping_processes(uint64_t now) {
    while (sleep_queue_head && now >= sleep_queue_head->sleep_until) {
        process_t* proc = sleep_queue_head;
        sleep_queue_head = proc->sleep_next;
        proc->sleep_next = NULL;

        if (proc->state == PROCESS_SLEEPING) {
            proc->state = PROCESS_READY;
        }
    }
}

static void save_running_context(process_t* current, uint64_t* interrupt_rsp) {
    if (!current || !interrupt_rsp) {
        return;
    }

    interrupt_context_t* frame = frame_to_ctx(interrupt_rsp);
    current->interrupt_context = *frame;
    current->has_been_interrupted = true;

    if (current->state == PROCESS_RUNNING && current->pid != 0) {
        current->state = PROCESS_READY;
    }
}

static void load_first_context(process_t* next, uint64_t* interrupt_rsp) {
    interrupt_context_t* frame = frame_to_ctx(interrupt_rsp);

    // Zero all registers so we start with a clean slate
    for (int i = 0; i < 15; i++) {
        interrupt_rsp[i] = 0;
    }

    frame->rip    = next->interrupt_context.rip;
    frame->cs     = next->interrupt_context.cs;
    frame->rflags = next->interrupt_context.rflags;
    frame->rsp    = next->interrupt_context.rsp;
    frame->ss     = next->interrupt_context.ss;

    // Preserve r12 for kernel threads so process_entry can grab the entry point
    frame->r12 = next->interrupt_context.r12;

    next->has_been_interrupted = true;
}

static void load_saved_context(process_t* next, uint64_t* interrupt_rsp) {
    interrupt_context_t* frame = frame_to_ctx(interrupt_rsp);
    *frame = next->interrupt_context;
}

static process_t* find_idle_process(void) {
    for (process_t* proc = process_get_list(); proc; proc = proc->next) {
        if (proc->pid == 0) {
            return proc;
        }
    }
    return NULL;
}

static void reset_timeslice(process_t* proc) {
    if (!proc) return;

    if (proc->time_slice_ticks == 0) {
        proc->time_slice_ticks = SCHED_DEFAULT_TIMESLICE_TICKS;
    }

    proc->time_slice_remaining = proc->time_slice_ticks;
}

static process_t* select_next_process(process_t* current) {
    process_t* head = process_get_list();
    if (!head) return NULL;

    process_t* start = current && current->next ? current->next : head;
    process_t* iter = start;
    process_t* best = NULL;
    int best_priority = INT_MIN;

    do {
        bool runnable = iter->state == PROCESS_READY || iter->state == PROCESS_RUNNING;
        if (runnable) {
            int priority = iter->priority;
            if (priority > best_priority || (!best && priority == best_priority)) {
                best = iter;
                best_priority = priority;
            }
        }

        iter = iter->next ? iter->next : head;
    } while (iter != start);

    if (best && best->priority > SCHED_IDLE_PRIORITY) {
        return best;
    }

    if (best) {
        return best; // likely the idle thread
    }

    return find_idle_process();
}

bool scheduler_sleep_until(uint64_t* interrupt_rsp, uint64_t target_tick) {
    process_t* current = process_current();
    if (!current) {
        return false;
    }

    interrupt_context_t* frame = interrupt_rsp ? frame_to_ctx(interrupt_rsp) : NULL;
    if (frame) {
        current->interrupt_context = *frame;
        current->interrupt_context.rax = 0; // nanosleep-style success return value
    }

    current->has_been_interrupted = true;
    current->sleep_until = target_tick;
    current->state = PROCESS_SLEEPING;
    current->sleep_interrupted = false;
    current->sleep_next = NULL;

    sleep_queue_insert(current);

    if (scheduler_reschedule(interrupt_rsp)) {
        return true; // Switched away; will resume when woken.
    }

    // Could not switch (should not happen with an idle task). Cancel the sleep.
    sleep_queue_remove(current);
    current->state = PROCESS_RUNNING;
    return false;
}

void scheduler_cancel_sleep(process_t* proc) {
    sleep_queue_remove(proc);
}

bool scheduler_reschedule(uint64_t* interrupt_rsp) {
    if (in_scheduler) return false;
    in_scheduler = true;

    process_t* current = process_current();
    if (!current) {
        in_scheduler = false;
        return false;
    }

    page_table_t* kernel_pt = vmm_get_kernel_page_table();
    page_table_t* current_pt = current->page_table ? current->page_table : kernel_pt;
    bool switched_to_kernel = false;

    if (current_pt != kernel_pt) {
        vmm_switch_page_table(kernel_pt);
        switched_to_kernel = true;
    }

    uint64_t now = timer_get_ticks();
    wake_sleeping_processes(now);
    process_cleanup_terminated();

    process_t* next = select_next_process(current);
    if (!next || next == current) {
        if (current && current->time_slice_remaining == 0) {
            reset_timeslice(current);
        }
        if (switched_to_kernel) {
            vmm_switch_page_table(current_pt);
        }
        in_scheduler = false;
        return false;
    }

    save_running_context(current, interrupt_rsp);

    next->state = PROCESS_RUNNING;

    reset_timeslice(next);

    if (next->has_been_interrupted) {
        load_saved_context(next, interrupt_rsp);
    } else {
        load_first_context(next, interrupt_rsp);
    }

    tss_set_kernel_stack(next->stack_top);

    page_table_t* target = next->page_table ? next->page_table
                                           : vmm_get_kernel_page_table();
    vmm_switch_page_table(target);

    process_set_current(next);

    in_scheduler = false;
    return true;
}

static void scheduler_tick_handler(uint64_t* interrupt_rsp) {
    uint64_t now = timer_get_ticks();
    wake_sleeping_processes(now);
    process_cleanup_terminated();

    process_t* current = process_current();
    if (!current) return;

    process_t* candidate = select_next_process(current);

    // Preempt immediately if a higher-priority task is ready.
    if (candidate && candidate != current && candidate->priority > current->priority) {
        scheduler_reschedule(interrupt_rsp);
        return;
    }

    // Drive time-sliced preemption for normal tasks (skip idle).
    if (current->pid != 0 && current->state == PROCESS_RUNNING) {
        if (current->time_slice_remaining > 0) {
            current->time_slice_remaining--;
        }

        if (current->time_slice_remaining == 0) {
            scheduler_reschedule(interrupt_rsp);
            return;
        }
    } else if (candidate && candidate != current && candidate->priority > SCHED_IDLE_PRIORITY) {
        // If we're idle but something else is runnable, try to switch.
        scheduler_reschedule(interrupt_rsp);
    }
}

void scheduler_init(void) {
    timer_register_tick_handler(scheduler_tick_handler);
}
