#include "core/process.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "arch/x86/tss.h"
#include "memory/vmm.h"
#include "drivers/timer.h"
#include "lib/string.h"
#include <stddef.h>

process_t* process_list_head = NULL;
static process_t* current_process = NULL;
static uint32_t next_pid = 1;

extern void switch_context(context_t* old_ctx, context_t* new_ctx);

static void process_reset_fd_table(process_t* proc) {
    if (!proc) return;
    for (size_t i = 0; i < PROCESS_MAX_FDS; i++) {
        proc->fd_table[i].in_use = false;
        proc->fd_table[i].data = NULL;
        proc->fd_table[i].size = 0;
        proc->fd_table[i].offset = 0;
        proc->fd_table[i].flags = 0;
        proc->fd_table[i].name[0] = '\0';
    }
    proc->fds_initialized = true;
}

static void process_reset_cwd(process_t* proc) {
    if (!proc) return;
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';
    proc->cwd_initialized = true;
}

static bool process_phys_is_reserved(process_t* proc, uint64_t phys) {
    if (!proc || proc->fb_mapping_size == 0) {
        return false;
    }

    uint64_t start = proc->fb_mapping_phys_base;
    uint64_t size = proc->fb_mapping_size;
    uint64_t end = start + size;

    if (end < start) {
        // Overflow - treat as not reserved
        return false;
    }

    return phys >= start && phys < end;
}

void process_init(void) {
    process_t* idle = (process_t*)kmalloc(sizeof(process_t));
    if (!idle) return;
    
    memset(idle, 0, sizeof(process_t));
    idle->pid = 0;
    idle->name[0] = 'i';
    idle->name[1] = 'd';
    idle->name[2] = 'l';
    idle->name[3] = 'e';
    idle->name[4] = '\0';
    idle->state = PROCESS_RUNNING;
    idle->next = NULL;

    idle->start_ticks = timer_get_ticks();

    process_reset_fd_table(idle);
    process_reset_cwd(idle);

    process_list_head = idle;
    current_process = idle;
}

void process_entry(void) {
    void (*entry_func)(void);
    asm volatile ("mov %%r12, %0" : "=r"(entry_func));
    
    entry_func();
    
    current_process->state = PROCESS_TERMINATED;
    
    // Try to find another ready process
    process_t* next = process_get_list();
    while (next) {
        if (next->state == PROCESS_READY) {
            process_switch_to(next);
            break;
        }
        next = next->next;
    }
    
    // If no ready process found, halt
    while(1) { asm volatile ("hlt"); }
}

process_t* process_create(const char* name, void (*entry_point)(void)) {
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) return NULL;
    
    memset(proc, 0, sizeof(process_t));

    proc->has_been_interrupted = false;

    proc->pid = next_pid++;
    proc->state = PROCESS_READY;
    proc->start_ticks = timer_get_ticks();

    process_reset_fd_table(proc);
    process_reset_cwd(proc);
    
    if (name) {
        size_t len = strlen(name);
        if (len >= 63) len = 63;
        for (size_t i = 0; i < len; i++) {
            proc->name[i] = name[i];
        }
        proc->name[len] = '\0';
    }
    
    uint64_t stack_phys = (uint64_t)pmm_alloc_pages(2);
    if (!stack_phys) {
        kfree(proc);
        return NULL;
    }
    
    uint64_t stack_base = (uint64_t)phys_to_virt(stack_phys);
    proc->stack_top = stack_base + (2 * PAGE_SIZE);
    
    uint64_t* stack = (uint64_t*)proc->stack_top;
    *(--stack) = (uint64_t)process_entry;
    
    proc->context.rsp = (uint64_t)stack;
    proc->context.rbp = 0;
    proc->context.rbx = 0;
    proc->context.r12 = (uint64_t)entry_point;
    proc->context.r13 = 0;
    proc->context.r14 = 0;
    proc->context.r15 = 0;
    proc->context.rflags = 0x202;
    
    proc->next = process_list_head;
    process_list_head = proc;
    
    return proc;
}

process_t* process_current(void) {
    return current_process;
}

process_t* process_get_list(void) {
    return process_list_head;
}

process_t* process_find_by_pid(uint32_t pid) {
    process_t* proc = process_list_head;
    while (proc) {
        if (proc->pid == pid) return proc;
        proc = proc->next;
    }
    return NULL;
}

bool process_kill(uint32_t pid) {
    if (pid == 0) return false;

    process_t* target = process_find_by_pid(pid);
    if (!target) return false;

    // Do not allow killing the current process from here to avoid tearing down
    // the active stack frame mid-command; callers can schedule away if needed.
    if (target == process_current()) {
        target->state = PROCESS_TERMINATED;
        return true;
    }

    target->state = PROCESS_TERMINATED;

    // Immediately clean up so the PID disappears from pslist without waiting
    // for the next timer interrupt.
    process_cleanup_terminated();
    return true;
}

void process_switch_to(process_t* next) {
    if (!next) return;
    if (next == process_current()) return;
    if (next->state != PROCESS_READY && next->state != PROCESS_RUNNING) return;

    process_t* old = process_current();
    if (old && old->state == PROCESS_RUNNING) old->state = PROCESS_READY;

    // Always load the target address space *before* old is eligible for cleanup.
    page_table_t* target = next->page_table ? next->page_table
                                            : vmm_get_kernel_page_table();
    vmm_switch_page_table(target);            // CR3 ← target
    tss_set_kernel_stack(next->stack_top);    // keep TSS in sync (safe even for kthreads)

    next->state = PROCESS_RUNNING;
    process_set_current(next);

    // Swap CPU register context
    switch_context(old ? &old->context : NULL, &next->context);

    // Now we are executing on 'next' CR3; it's safe to collect dead processes.
    process_cleanup_terminated();
}

__attribute__((naked))
void switch_context(context_t* old_ctx __attribute__((unused)), 
                    context_t* new_ctx __attribute__((unused))) {
    asm volatile (
        "mov %rsp, 0(%rdi)\n"
        "mov %rbp, 8(%rdi)\n"
        "mov %rbx, 16(%rdi)\n"
        "mov %r12, 24(%rdi)\n"
        "mov %r13, 32(%rdi)\n"
        "mov %r14, 40(%rdi)\n"
        "mov %r15, 48(%rdi)\n"
        "pushfq\n"
        "pop %rax\n"
        "mov %rax, 56(%rdi)\n"
        
        "mov 0(%rsi), %rsp\n"
        "mov 8(%rsi), %rbp\n"
        "mov 16(%rsi), %rbx\n"
        "mov 24(%rsi), %r12\n"
        "mov 32(%rsi), %r13\n"
        "mov 40(%rsi), %r14\n"
        "mov 48(%rsi), %r15\n"
        "mov 56(%rsi), %rax\n"
        "push %rax\n"
        "popfq\n"
        
        "ret\n"
    );
}

// Helper to free a page table recursively
static void free_page_table_recursive(uint64_t* table, int level) {
    if (!table || level < 1 || level > 4) return;

    // Don't free kernel mappings (upper half of PML4)
    int max_entry = (level == 4) ? 256 : 512;

    for (int i = 0; i < max_entry; i++) {
        uint64_t entry = table[i];
        if (!(entry & PAGE_PRESENT)) continue;

        uint64_t phys = entry & 0x000FFFFFFFFFF000ULL;

        if (level > 1) {
            uint64_t* next_table = phys_to_virt(phys);
            free_page_table_recursive(next_table, level - 1);
            pmm_free((void*)phys);
        }
    }
}

void process_free_page_table(page_table_t* pt) {
    if (!pt) return;

    free_page_table_recursive(pt->pml4_virt, 4);
    pmm_free((void*)pt->pml4_phys);
    // pt itself was allocated from the kernel heap (kmalloc/kcalloc), not as a
    // standalone PMM page. Freeing it via pmm_free() corrupts the heap because
    // it marks the whole underlying page as free even if it contains other
    // allocations.
    kfree(pt);
}

void process_destroy(process_t* proc) {
    if (!proc) return;
    extern void syscall_on_process_exit(process_t* proc_ref);
    syscall_on_process_exit(proc);
    
    // Free kernel stack
    if (proc->stack_top) {
        uint64_t stack_base = proc->stack_top - (2 * PAGE_SIZE);
        uint64_t stack_phys = virt_to_phys((void*)stack_base);
        pmm_free_pages((void*)stack_phys, 2);
    }
    
    // Free user stack if this is a usermode process
    if (proc->is_usermode && proc->user_stack_top) {
        uint64_t user_stack_base = proc->user_stack_top - (4 * PAGE_SIZE);
        // Need to get physical address through page table
        if (proc->page_table) {
            for (int i = 0; i < 4; i++) {
                uint64_t virt = user_stack_base + (i * PAGE_SIZE);
                uint64_t phys = vmm_get_physical(proc->page_table, virt);
                if (phys && !process_phys_is_reserved(proc, phys)) {
                    pmm_free((void*)phys);
                }
            }
        }
    }
    
    // Free heap pages
    if (proc->is_usermode && proc->page_table && proc->heap_end > proc->heap_start) {
        uint64_t heap_pages = (proc->heap_end - proc->heap_start + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t i = 0; i < heap_pages; i++) {
            uint64_t virt = proc->heap_start + (i * PAGE_SIZE);
            uint64_t phys = vmm_get_physical(proc->page_table, virt);
            if (phys && !process_phys_is_reserved(proc, phys)) {
                pmm_free((void*)phys);
            }
        }
    }
    
    // Free ELF segment pages by walking the actual page table structure
    if (proc->is_usermode && proc->page_table) {
        uint64_t* pml4 = proc->page_table->pml4_virt;
        
        // Walk only the lower half (entries 0-255)
        for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
            if (!(pml4[pml4_idx] & PAGE_PRESENT)) continue;
            
            uint64_t* pdpt = phys_to_virt(pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);
            for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
                if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) continue;
                
                uint64_t* pd = phys_to_virt(pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL);
                for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                    if (!(pd[pd_idx] & PAGE_PRESENT)) continue;
                    
                    uint64_t* pt = phys_to_virt(pd[pd_idx] & 0x000FFFFFFFFFF000ULL);
                    for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                        if (!(pt[pt_idx] & PAGE_PRESENT)) continue;
                        
                        uint64_t virt = ((uint64_t)pml4_idx << 39) | 
                                    ((uint64_t)pdpt_idx << 30) |
                                    ((uint64_t)pd_idx << 21) |
                                    ((uint64_t)pt_idx << 12);
                        
                        // Skip stack and heap regions
                        if (virt >= proc->heap_start && virt < proc->heap_end) continue;
                        if (proc->user_stack_top &&
                            virt >= (proc->user_stack_top - (4 * PAGE_SIZE)) &&
                            virt < proc->user_stack_top) continue;

                        uint64_t phys = pt[pt_idx] & 0x000FFFFFFFFFF000ULL;
                        if (!process_phys_is_reserved(proc, phys)) {
                            pmm_free((void*)phys);
                        }
                    }
                }
            }
        }
    }
    
    // Free page table structure
    if (proc->page_table) {
        process_free_page_table(proc->page_table);
        proc->page_table = NULL;
    }
    
    // Remove from process list
    if (process_list_head == proc) {
        process_list_head = proc->next;
    } else {
        process_t* current = process_list_head;
        while (current && current->next != proc) {
            current = current->next;
        }
        if (current) {
            current->next = proc->next;
        }
    }
    
    // Free the process structure
    kfree(proc);
}

void process_cleanup_terminated(void) {
    process_t* proc = process_list_head;
    process_t* prev = NULL;
    
    while (proc) {
        process_t* next = proc->next;
        
        if (proc->state == PROCESS_TERMINATED && proc != process_current()) {
            // Unlink from list first
            if (prev) {
                prev->next = next;
            } else {
                process_list_head = next;
            }
            
            // Now destroy it (this will free all resources)
            process_destroy(proc);
            
            // Don't update prev since we removed this node
            proc = next;
        } else {
            prev = proc;
            proc = next;
        }
    }
}

__attribute__((naked))
void enter_usermode(uint64_t, uint64_t) {
    asm volatile (
        "mov $0x23, %ax\n"
        "mov %ax, %ds\n"
        "mov %ax, %es\n"
        "mov %ax, %fs\n"
        "mov %ax, %gs\n"
        
        "push $0x23\n"
        "push %rsi\n"
        "push $0x202\n"
        "push $0x1B\n"
        "push %rdi\n"
        
        "iretq\n"
    );
}

void process_entry_usermode(void) {
    process_t* proc = process_current();
    if (!proc) {
        while (1) { asm volatile ("hlt"); }
    }
    
    // Set kernel stack for when we return from usermode
    tss_set_kernel_stack(proc->stack_top);
    
    // Switch to process's page table
    if (proc->page_table) {
        vmm_switch_page_table(proc->page_table);
    }
    
    // Drop to ring 3.
    // Use the process' saved interrupt context rather than always starting at
    // user_stack_top. This is critical for Linux/SysV ABI startup where argc,
    // argv, envp, etc. live on the initial user stack.
    uint64_t entry = proc->interrupt_context.rip;
    uint64_t user_rsp = proc->interrupt_context.rsp;
    if (entry == 0) {
        // Fallback for older callers that only seeded r12.
        asm volatile ("mov %%r12, %0" : "=r"(entry));
    }
    if (user_rsp == 0) {
        user_rsp = proc->user_stack_top;
    }
    enter_usermode(entry, user_rsp);
    
    // Never returns
}

void process_set_current(process_t* proc) {
    current_process = proc;
}