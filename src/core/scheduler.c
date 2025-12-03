#include "core/scheduler.h"
#include "arch/x86/tss.h"
#include "core/process.h"
#include "drivers/timer.h"
#include "memory/vmm.h"

#include <stdbool.h>
#include <stdint.h>

static volatile bool in_scheduler = false;

static inline interrupt_context_t* frame_to_ctx(uint64_t* interrupt_rsp) {
    return (interrupt_context_t*)interrupt_rsp;
}

static void wake_sleeping_processes(uint64_t now) {
    for (process_t* proc = process_get_list(); proc; proc = proc->next) {
        if (proc->state == PROCESS_SLEEPING && now >= proc->sleep_until) {
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

static process_t* select_next_process(process_t* current) {
    process_t* head = process_get_list();
    if (!head) return NULL;

    process_t* start = current && current->next ? current->next : head;
    process_t* iter = start;

    do {
        if (iter->state == PROCESS_READY && iter->pid != 0) {
            return iter;
        }

        iter = iter->next ? iter->next : head;
    } while (iter != start);

    return find_idle_process();
}

bool scheduler_reschedule(uint64_t* interrupt_rsp) {
    if (in_scheduler) return false;
    in_scheduler = true;

    process_t* current = process_current();
    if (!current) {
        in_scheduler = false;
        return false;
    }

    uint64_t now = timer_get_ticks();
    wake_sleeping_processes(now);
    process_cleanup_terminated();

    process_t* next = select_next_process(current);
    if (!next || next == current) {
        in_scheduler = false;
        return false;
    }

    save_running_context(current, interrupt_rsp);

    next->state = PROCESS_RUNNING;

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
    // Use the interrupt frame to drive preemption. The timer handler only
    // invokes us periodically, so we simply attempt to reschedule each time.
    scheduler_reschedule(interrupt_rsp);
}

void scheduler_init(void) {
    timer_register_tick_handler(scheduler_tick_handler);
}
