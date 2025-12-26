#ifndef CORE_PROCESS_H
#define CORE_PROCESS_H

#include <stdint.h>
#include <stdbool.h>

#include "memory/vmm.h"

typedef enum {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_SLEEPING,
    PROCESS_TERMINATED
} process_state_t;

typedef struct {
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rflags;
} context_t;

// Full interrupt context (for timer-based preemption)
typedef struct { 
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_context_t;

#define PROCESS_MAX_FDS 32

typedef struct {
    bool in_use;
    void* data;
    size_t size;
    size_t offset;
    int flags;
    char name[64];
} fd_entry_t;

typedef struct process {
    uint32_t pid;
    char name[64];
    process_state_t state;

    context_t context;                     // For voluntary context switches
    interrupt_context_t interrupt_context; // For timer interrupts
    uint64_t stack_top;                    // Kernel stack (for syscalls/interrupts)
    uint64_t user_stack_top;               // User stack (for running user code)

    uint64_t heap_start;                   // Start of heap (right after code/data)
    uint64_t heap_end;                     // Current end of heap (brk point)

    page_table_t* page_table;              // Per-process page table (NULL = use kernel PT)
    bool is_usermode;                      // True if this runs in ring 3
    bool has_been_interrupted;             // True after first timer interrupt

    // Track device mappings that must not be returned to the PMM
    uint64_t fb_mapping_phys_base;
    uint64_t fb_mapping_size;
    uint64_t fb_mapping_virt_base;

    uint64_t start_ticks;                  // Ticks at process start (for timing)

    uint64_t sleep_until;                  // Target tick to wake from sleep
    bool sleep_interrupted;                // True if a sleep was cut short

    int last_errno;                        // errno-like value for syscalls

    fd_entry_t fd_table[PROCESS_MAX_FDS];  // Per-process file descriptors
    bool fds_initialized;

    char cwd[512];                         // Per-process working directory
    bool cwd_initialized;

    struct process* next;
} process_t;

void process_init(void);
process_t* process_create(const char* name, void (*entry_point)(void));
process_t* process_current(void);
process_t* process_get_list(void);
void process_switch_to(process_t* next);
bool process_kill(uint32_t pid);
process_t* process_find_by_pid(uint32_t pid);

extern process_t* process_list_head;
void process_destroy(process_t* proc);
void process_cleanup_terminated(void);
void enter_usermode(uint64_t entry, uint64_t user_stack);
extern void process_entry(void);
extern void process_entry_usermode(void);
void process_set_current(process_t* proc);
void process_free_page_table(page_table_t* pt);

#endif // CORE_PROCESS_H
