#include "core/scheduler.h"
#include "core/process.h"
#include "drivers/timer.h"
#include "arch/x86/tss.h"
#include "memory/vmm.h"
#include <stdbool.h>
#include <stdint.h>

static volatile bool in_scheduler = false;

static void scheduler_tick_handler(uint64_t* interrupt_rsp) {
    if (in_scheduler) return;
    in_scheduler = true;

    process_t* current = process_current();
    if (!current || current->pid == 0) {
        in_scheduler = false;
        return;
    }

    if (current->is_usermode && !current->has_been_interrupted) {
        uint64_t cs = interrupt_rsp[16];
        if (cs == 0x1B) {
            current->has_been_interrupted = true;
        } else {
            in_scheduler = false;
            return;
        }
    }

    process_cleanup_terminated();

    int ready_count = 0;
    process_t* p = process_get_list();
    while (p) {
        if (p != current && p->state == PROCESS_READY && p->pid != 0) {
            ready_count++;
        }
        p = p->next;
    }

    if (ready_count == 0) {
        in_scheduler = false;
        return;
    }

    process_t* next = current->next;
    if (!next) next = process_get_list();

    int checked = 0;
    while (next && checked < 10) {
        if (next != current && next->state == PROCESS_READY && next->pid != 0) {
            if (!next->has_been_interrupted) {
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

                current->state = PROCESS_READY;

                for (int i = 0; i < 15; i++) interrupt_rsp[i] = 0;
                interrupt_rsp[15] = next->interrupt_context.rip;
                interrupt_rsp[16] = next->interrupt_context.cs;
                interrupt_rsp[17] = next->interrupt_context.rflags;
                interrupt_rsp[18] = next->interrupt_context.rsp;
                interrupt_rsp[19] = next->interrupt_context.ss;

                next->state = PROCESS_RUNNING;
                next->has_been_interrupted = true;

                tss_set_kernel_stack(next->stack_top);

                if (next->page_table) {
                    vmm_switch_page_table(next->page_table);
                }

                process_set_current(next);

                in_scheduler = false;
                return;
            }

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

            current->state = PROCESS_READY;
            next->state = PROCESS_RUNNING;

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

            tss_set_kernel_stack(next->stack_top);

            if (next->page_table) {
                vmm_switch_page_table(next->page_table);
            }

            process_set_current(next);

            in_scheduler = false;
            return;
        }

        next = next->next;
        if (!next) next = process_get_list();
        checked++;
    }

    in_scheduler = false;
}

void scheduler_init(void) {
    timer_register_tick_handler(scheduler_tick_handler);
}
