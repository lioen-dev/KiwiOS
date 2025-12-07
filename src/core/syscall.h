#ifndef CORE_SYSCALL_H
#define CORE_SYSCALL_H

#include <stdint.h>

// System call numbers (Linux-compatible)
#define SYS_read      0
#define SYS_write     1
#define SYS_open      2
#define SYS_close     3
#define SYS_stat      4
#define SYS_fstat     5
#define SYS_lseek     8
#define SYS_mmap      9
#define SYS_mprotect  10
#define SYS_munmap    11
#define SYS_brk       12
#define SYS_ioctl     16
#define SYS_access    21
#define SYS_exit      60
#define SYS_fork      57
#define SYS_execve    59
#define SYS_wait4     61
#define SYS_kill      62
#define SYS_getpid    39
#define SYS_getppid   110
#define SYS_chdir     80
#define SYS_getcwd    79

// File flags
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

// Syscall context (saved on syscall entry)
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) syscall_context_t;

// Initialize syscall handling
void syscall_init(void);

// Update syscall kernel stack (call when switching processes)
void syscall_set_kernel_stack(uint64_t stack);

// Syscall handler (called from assembly)
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5);

#endif // CORE_SYSCALL_H