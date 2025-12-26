#include "core/scheduler.h"
#include "core/process.h"
#include "drivers/timer.h"
#include "arch/x86/tss.h"
#include "memory/vmm.h"
#include <stdbool.h>
#include <stdint.h>

static volatile bool in_scheduler = false;

static void wake_sleeping_processes(void) {
    uint64_t now = timer_get_ticks();
    process_t* wake = process_get_list();
    while (wake) {
        if (wake->state == PROCESS_SLEEPING && now >= wake->sleep_until) {
            wake->state = PROCESS_READY;
        }
        wake = wake->next;
    }
}

static void scheduler_tick_handler(uint64_t* interrupt_rsp) {
    if (in_scheduler) return;
    in_scheduler = true;

    // Wake sleeping processes whose deadlines have passed, even if the
    // current task is the idle process. Otherwise sleepers would never
    // transition back to READY when the scheduler is idle.
    wake_sleeping_processes();

    process_cleanup_terminated();

    process_t* current = process_current();
    if (!current) {
        in_scheduler = false;
        return;
    }

    // Our IRQ0 stub only has a full iret frame (RIP/CS/RFLAGS/RSP/SS)
    // when we interrupted ring 3. If we try to schedule while running in
    // ring 0 (e.g., inside a syscall), interrupt_rsp[18]/[19] are not valid
    // and we'll corrupt the stack.
    uint64_t cs = interrupt_rsp[16];
    if ((cs & 0x3) != 0x3) {
        in_scheduler = false;
        return;
    }

    // Find the next READY *usermode* process (round-robin).
    process_t* next = current->next;
    if (!next) next = process_get_list();
    process_t* start = next;
    while (next) {
        if (next != current && next->state == PROCESS_READY && next->pid != 0 && next->is_usermode) {
            break;
        }
        next = next->next;
        if (!next) next = process_get_list();
        if (next == start) {
            next = NULL;
            break;
        }
    }

    if (!next) {
        in_scheduler = false;
        return;
    }

    // Save current interrupt frame.
    current->interrupt_context.r15 = interrupt_rsp[0];
    current->interrupt_context.r14 = interrupt_rsp[1];
    current->interrupt_context.r13 = interrupt_rsp[2];
    current->interrupt_context.r12 = interrupt_rsp[3];
    current->interrupt_context.r11 = interrupt_rsp[4];
    current->interrupt_context.r10 = interrupt_rsp[5];
    current->interrupt_context.r9  = interrupt_rsp[6];
    current->interrupt_context.r8  = interrupt_rsp[7];
    current->interrupt_context.rbp = interrupt_rsp[8];
    current->interrupt_context.rdi = interrupt_rsp[9];
    current->interrupt_context.rsi = interrupt_rsp[10];
    current->interrupt_context.rdx = interrupt_rsp[11];
    current->interrupt_context.rcx = interrupt_rsp[12];
    current->interrupt_context.rbx = interrupt_rsp[13];
    current->interrupt_context.rax = interrupt_rsp[14];
    current->interrupt_context.rip = interrupt_rsp[15];
    current->interrupt_context.cs = interrupt_rsp[16];
    current->interrupt_context.rflags = interrupt_rsp[17];
    current->interrupt_context.rsp = interrupt_rsp[18];
    current->interrupt_context.ss = interrupt_rsp[19];
    current->has_been_interrupted = true;

    if (current->state == PROCESS_RUNNING) {
        current->state = PROCESS_READY;
    }

    // Restore next interrupt frame (this also preserves initial argc/argv
    // register seeding from elf_load_with_args).
    interrupt_rsp[0] = next->interrupt_context.r15;
    interrupt_rsp[1] = next->interrupt_context.r14;
    interrupt_rsp[2] = next->interrupt_context.r13;
    interrupt_rsp[3] = next->interrupt_context.r12;
    interrupt_rsp[4] = next->interrupt_context.r11;
    interrupt_rsp[5] = next->interrupt_context.r10;
    interrupt_rsp[6] = next->interrupt_context.r9;
    interrupt_rsp[7] = next->interrupt_context.r8;
    interrupt_rsp[8] = next->interrupt_context.rbp;
    interrupt_rsp[9] = next->interrupt_context.rdi;
    interrupt_rsp[10] = next->interrupt_context.rsi;
    interrupt_rsp[11] = next->interrupt_context.rdx;
    interrupt_rsp[12] = next->interrupt_context.rcx;
    interrupt_rsp[13] = next->interrupt_context.rbx;
    interrupt_rsp[14] = next->interrupt_context.rax;
    interrupt_rsp[15] = next->interrupt_context.rip;
    interrupt_rsp[16] = next->interrupt_context.cs;
    interrupt_rsp[17] = next->interrupt_context.rflags;
    interrupt_rsp[18] = next->interrupt_context.rsp;
    interrupt_rsp[19] = next->interrupt_context.ss;

    next->state = PROCESS_RUNNING;
    next->has_been_interrupted = true;

    tss_set_kernel_stack(next->stack_top);

    page_table_t* target = next->page_table ? next->page_table : vmm_get_kernel_page_table();
    vmm_switch_page_table(target);
    process_set_current(next);

    in_scheduler = false;
    return;
}

void scheduler_init(void) {
    timer_register_tick_handler(scheduler_tick_handler);
}
