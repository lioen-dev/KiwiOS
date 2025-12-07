#include "core/process.h"
#include "core/elf.h"
#include "core/syscall.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "arch/x86/tss.h"
#include "lib/string.h"

static process_t* process_table[MAX_PROCESSES];
static process_t* current_process = NULL;
static int next_pid = 1;

void process_init(void) {
    memset(process_table, 0, sizeof(process_table));
    current_process = NULL;
    
    // Create idle process (PID 0)
    process_t* idle = process_create("idle", true);
    if (idle) {
        idle->pid = 0;
        idle->state = PROC_STATE_READY;
    }
}

int process_alloc_pid(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        int pid = next_pid;
        next_pid = (next_pid + 1) % MAX_PROCESSES;
        if (next_pid == 0) next_pid = 1; // Skip PID 0
        
        if (!process_table[pid]) {
            return pid;
        }
    }
    return -1;
}

process_t* process_create(const char* name, bool kernel_mode) {
    int pid = process_alloc_pid();
    if (pid < 0) return NULL;
    
    process_t* proc = (process_t*)kcalloc(1, sizeof(process_t));
    if (!proc) return NULL;
    
    proc->pid = pid;
    proc->ppid = current_process ? current_process->pid : 0;
    proc->state = PROC_STATE_READY;
    
    if (name) {
        size_t len = strlen(name);
        if (len >= PROC_NAME_LEN) len = PROC_NAME_LEN - 1;
        memcpy(proc->name, name, len);
        proc->name[len] = '\0';
    }
    
    // Create page table
    if (!kernel_mode) {
        proc->page_table = vmm_create_page_table();
        if (!proc->page_table) {
            kfree(proc);
            return NULL;
        }
    } else {
        proc->page_table = vmm_get_kernel_page_table();
    }
    
    // Allocate kernel stack (8 pages = 32KB)
    void* kstack_phys = pmm_alloc_pages(8);
    if (!kstack_phys) {
        if (!kernel_mode && proc->page_table) {
            // TODO: Free page table
        }
        kfree(proc);
        return NULL;
    }
    
    proc->kernel_stack = (uint64_t)phys_to_virt((uint64_t)kstack_phys) + (8 * PAGE_SIZE);
    
    // Initialize CWD
    strcpy(proc->cwd, "/");
    
    // Initialize FDs
    for (int i = 0; i < MAX_FDS; i++) {
        proc->fds[i].used = false;
    }
    
    // Add to process table
    process_table[pid] = proc;
    
    return proc;
}

void process_destroy(process_t* proc) {
    if (!proc) return;
    
    // Remove from process table
    if (proc->pid >= 0 && proc->pid < MAX_PROCESSES) {
        process_table[proc->pid] = NULL;
    }
    
    // Free kernel stack
    if (proc->kernel_stack) {
        uint64_t stack_base = proc->kernel_stack - (8 * PAGE_SIZE);
        pmm_free_pages((void*)virt_to_phys((void*)stack_base), 8);
    }
    
    // TODO: Free page table and all mapped pages
    
    kfree(proc);
}

process_t* process_current(void) {
    return current_process;
}

void process_set_current(process_t* proc) {
    current_process = proc;
    
    if (proc) {
        // Update TSS with kernel stack
        tss_set_kernel_stack(proc->kernel_stack);
        
        // Update syscall kernel stack
        syscall_set_kernel_stack(proc->kernel_stack);
        
        // Switch page table if userspace process
        if (proc->page_table != vmm_get_kernel_page_table()) {
            vmm_switch_page_table(proc->page_table);
        }
    }
}

process_t* process_get(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return NULL;
    return process_table[pid];
}

int process_exec(const char* path, char** argv, char** envp) {
    if (!current_process) return -1;
    
    // Load new ELF
    process_t* new_proc = elf_load(path, argv, envp);
    if (!new_proc) return -1;
    
    // Replace current process with new one (keep PID)
    int old_pid = current_process->pid;
    int old_ppid = current_process->ppid;
    
    process_destroy(current_process);
    
    new_proc->pid = old_pid;
    new_proc->ppid = old_ppid;
    process_table[old_pid] = new_proc;
    current_process = new_proc;
    
    // Jump to userspace
    process_enter_userspace(new_proc->context.rip, new_proc->context.rsp);
    
    return 0; // Never reached
}

__attribute__((noreturn)) void process_exit(int code) {
    if (current_process) {
        current_process->state = PROC_STATE_ZOMBIE;
        current_process->exit_code = code;
    }
    
    // TODO: Wake up parent waiting for us
    
    // Schedule next process
    for (;;) {
        asm volatile("hlt");
    }
}

// Enter userspace (called from kernel)
void process_enter_userspace(uint64_t entry, uint64_t user_stack) {
    asm volatile(
        "mov %0, %%rsp\n"     // Set user stack
        "push $0x23\n"        // SS (user data segment)
        "push %0\n"           // RSP
        "push $0x202\n"       // RFLAGS (IF=1)
        "push $0x1B\n"        // CS (user code segment)
        "push %1\n"           // RIP
        "iretq\n"
        :
        : "r"(user_stack), "r"(entry)
        : "memory"
    );
    
    __builtin_unreachable();
}