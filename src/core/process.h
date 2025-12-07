#ifndef CORE_PROCESS_H
#define CORE_PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include "memory/vmm.h"

#define MAX_PROCESSES 256
#define MAX_FDS 256
#define PROC_NAME_LEN 64

typedef enum {
    PROC_STATE_UNUSED = 0,
    PROC_STATE_READY,
    PROC_STATE_RUNNING,
    PROC_STATE_BLOCKED,
    PROC_STATE_ZOMBIE
} proc_state_t;

// CPU context saved during context switch
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) cpu_context_t;

// File descriptor
typedef struct {
    bool used;
    int type; // 0=unused, 1=file, 2=pipe, etc.
    void* data;
} fd_t;

// Process control block
typedef struct process {
    int pid;
    int ppid;
    proc_state_t state;
    char name[PROC_NAME_LEN];
    
    // Memory management
    page_table_t* page_table;
    uint64_t brk;              // Current program break for heap
    uint64_t brk_start;        // Initial program break
    
    // CPU context
    cpu_context_t context;
    uint64_t kernel_stack;     // Kernel stack for this process
    uint64_t user_stack;       // User stack pointer
    
    // File descriptors
    fd_t fds[MAX_FDS];
    char cwd[256];
    
    // Scheduling
    uint64_t time_slice;
    uint64_t total_time;
    
    // Exit status
    int exit_code;
    
    struct process* next;
} process_t;

// Initialize process subsystem
void process_init(void);

// Create a new process
process_t* process_create(const char* name, bool kernel_mode);

// Destroy a process
void process_destroy(process_t* proc);

// Get current process
process_t* process_current(void);

// Set current process
void process_set_current(process_t* proc);

// Get process by PID
process_t* process_get(int pid);

// Allocate a new PID
int process_alloc_pid(void);

// Fork current process
int process_fork(void);

// Execute a new program
int process_exec(const char* path, char** argv, char** envp);

// Exit current process
__attribute__((noreturn)) void process_exit(int code);

// Wait for child process
int process_wait(int pid, int* status);

// Scheduler integration
void process_schedule(cpu_context_t* ctx);

// Switch to userspace
void process_enter_userspace(uint64_t entry, uint64_t user_stack);

#endif // CORE_PROCESS_H