#include "core/syscall.h"
#include "core/process.h"
#include "fs/ext2.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "lib/string.h"

extern ext2_fs_t* g_fs;

// Syscall: read(fd, buf, count)
static int64_t sys_read(int fd, void* buf, size_t count) {
    (void)buf; (void)count; // TODO: Implement
    process_t* proc = process_current();
    if (!proc || fd < 0 || fd >= MAX_FDS || !proc->fds[fd].used) {
        return -1; // EBADF
    }
    
    // TODO: Implement actual file reading
    // For now, return 0 (EOF)
    return 0;
}

// Syscall: write(fd, buf, count)
static int64_t sys_write(int fd, const void* buf, size_t count) {
    (void)buf; // TODO: Implement actual console write
    if (fd == 1 || fd == 2) { // stdout/stderr
        // TODO: Write to console
        // For now, just return count to indicate success
        return (int64_t)count;
    }
    
    process_t* proc = process_current();
    if (!proc || fd < 0 || fd >= MAX_FDS || !proc->fds[fd].used) {
        return -1; // EBADF
    }
    
    // TODO: Implement actual file writing
    return (int64_t)count;
}

// Syscall: open(path, flags, mode)
static int64_t sys_open(const char* path, int flags, int mode) {
    (void)flags; (void)mode; // TODO: Implement
    process_t* proc = process_current();
    if (!proc || !path) return -1;
    
    // Find free FD
    int fd = -1;
    for (int i = 3; i < MAX_FDS; i++) { // Reserve 0,1,2 for stdin/out/err
        if (!proc->fds[i].used) {
            fd = i;
            break;
        }
    }
    
    if (fd < 0) return -1; // EMFILE
    
    // TODO: Actual file opening logic
    proc->fds[fd].used = true;
    proc->fds[fd].type = 1; // Regular file
    
    return fd;
}

// Syscall: close(fd)
static int64_t sys_close(int fd) {
    process_t* proc = process_current();
    if (!proc || fd < 0 || fd >= MAX_FDS || !proc->fds[fd].used) {
        return -1; // EBADF
    }
    
    proc->fds[fd].used = false;
    proc->fds[fd].type = 0;
    
    return 0;
}

// Syscall: brk(addr)
static int64_t sys_brk(uint64_t addr) {
    process_t* proc = process_current();
    if (!proc) return -1;
    
    if (addr == 0) {
        // Query current brk
        return (int64_t)proc->brk;
    }
    
    // Align to page boundary
    uint64_t new_brk = PAGE_ALIGN_UP(addr);
    uint64_t old_brk = proc->brk;
    
    if (new_brk > old_brk) {
        // Expand heap
        for (uint64_t va = old_brk; va < new_brk; va += PAGE_SIZE) {
            void* phys = pmm_alloc();
            if (!phys) return (int64_t)old_brk;
            
            memset(phys_to_virt((uint64_t)phys), 0, PAGE_SIZE);
            
            if (!vmm_map_page(proc->page_table, va, (uint64_t)phys,
                             PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NO_EXEC)) {
                pmm_free(phys);
                return (int64_t)old_brk;
            }
        }
    } else if (new_brk < old_brk) {
        // Shrink heap
        for (uint64_t va = new_brk; va < old_brk; va += PAGE_SIZE) {
            uint64_t phys = vmm_get_physical(proc->page_table, va);
            if (phys) {
                vmm_unmap_page(proc->page_table, va);
                pmm_free((void*)phys);
            }
        }
    }
    
    proc->brk = new_brk;
    return (int64_t)new_brk;
}

// Syscall: exit(code)
static int64_t sys_exit(int code) {
    process_exit(code);
    return 0; // Never reached
}

// Syscall: getpid()
static int64_t sys_getpid(void) {
    process_t* proc = process_current();
    return proc ? proc->pid : -1;
}

// Syscall: getppid()
static int64_t sys_getppid(void) {
    process_t* proc = process_current();
    return proc ? proc->ppid : -1;
}

// Syscall: chdir(path)
static int64_t sys_chdir(const char* path) {
    process_t* proc = process_current();
    if (!proc || !path || !g_fs) return -1;
    
    if (ext2_chdir(g_fs, path)) {
        const char* new_cwd = ext2_get_cwd();
        if (new_cwd) {
            size_t len = strlen(new_cwd);
            if (len >= sizeof(proc->cwd)) len = sizeof(proc->cwd) - 1;
            memcpy(proc->cwd, new_cwd, len);
            proc->cwd[len] = '\0';
        }
        return 0;
    }
    
    return -1;
}

// Syscall: getcwd(buf, size)
static int64_t sys_getcwd(char* buf, size_t size) {
    process_t* proc = process_current();
    if (!proc || !buf || size == 0) return -1;
    
    size_t len = strlen(proc->cwd);
    if (len + 1 > size) return -1; // ERANGE
    
    memcpy(buf, proc->cwd, len + 1);
    return (int64_t)buf;
}

// Main syscall dispatcher
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg4; (void)arg5; // Not used yet
    int64_t ret = -1;
    
    switch (syscall_num) {
        case SYS_read:
            ret = sys_read((int)arg1, (void*)arg2, (size_t)arg3);
            break;
        
        case SYS_write:
            ret = sys_write((int)arg1, (const void*)arg2, (size_t)arg3);
            break;
        
        case SYS_open:
            ret = sys_open((const char*)arg1, (int)arg2, (int)arg3);
            break;
        
        case SYS_close:
            ret = sys_close((int)arg1);
            break;
        
        case SYS_brk:
            ret = sys_brk(arg1);
            break;
        
        case SYS_exit:
            ret = sys_exit((int)arg1);
            break;
        
        case SYS_getpid:
            ret = sys_getpid();
            break;
        
        case SYS_getppid:
            ret = sys_getppid();
            break;
        
        case SYS_chdir:
            ret = sys_chdir((const char*)arg1);
            break;
        
        case SYS_getcwd:
            ret = sys_getcwd((char*)arg1, (size_t)arg2);
            break;
        
        default:
            ret = -1; // ENOSYS
            break;
    }
    
    return (uint64_t)ret;
}

// Assembly syscall entry point
__attribute__((naked)) void syscall_entry(void) {
    asm volatile(
        // Save user RSP and load kernel RSP
        // We'll use a global for now (simpler than GS)
        "mov %rsp, syscall_user_rsp(%rip)\n"
        "mov syscall_kernel_rsp(%rip), %rsp\n"
        
        // Push user context
        "push syscall_user_rsp(%rip)\n"  // User RSP
        "push %r11\n"                      // User RFLAGS
        "push %rcx\n"                      // User RIP
        
        // Push callee-saved registers
        "push %rbp\n"
        "push %rbx\n"
        "push %r12\n"
        "push %r13\n"
        "push %r14\n"
        "push %r15\n"
        
        // Syscall arguments are in: rax, rdi, rsi, rdx, r10, r8, r9
        // Move to C calling convention: rdi, rsi, rdx, rcx, r8, r9
        "mov %r10, %rcx\n"          // arg3: r10 -> rcx
        // rax already has syscall num, will go to rdi
        // rdi already has arg1, will go to rsi  
        // rsi already has arg2, will go to rdx
        // rdx already has arg3, will go to rcx (done above)
        // r10 has arg4, now in rcx, need to move to r8
        // r8 has arg5, will go to r9
        
        "mov %r8, %r9\n"            // arg5: r8 -> r9
        "mov %rcx, %r8\n"           // arg4: rcx -> r8
        "mov %rdx, %rcx\n"          // arg3: rdx -> rcx
        "mov %rsi, %rdx\n"          // arg2: rsi -> rdx
        "mov %rdi, %rsi\n"          // arg1: rdi -> rsi
        "mov %rax, %rdi\n"          // syscall_num: rax -> rdi
        
        // Call handler
        "call syscall_handler\n"
        
        // Restore callee-saved registers
        "pop %r15\n"
        "pop %r14\n"
        "pop %r13\n"
        "pop %r12\n"
        "pop %rbx\n"
        "pop %rbp\n"
        
        // Restore user context
        "pop %rcx\n"                // User RIP
        "pop %r11\n"                // User RFLAGS  
        "pop %rsp\n"                // User RSP
        
        "sysretq\n"
    );
}

// Global storage for syscall RSP values
uint64_t syscall_user_rsp;
uint64_t syscall_kernel_rsp;

void syscall_init(void) {
    // Enable SYSCALL/SYSRET instructions
    
    // Set up syscall kernel stack (will be updated per-process later)
    extern uint64_t syscall_kernel_rsp;
    process_t* proc = process_current();
    if (proc) {
        syscall_kernel_rsp = proc->kernel_stack;
    }
    
    // Set up STAR MSR (CS/SS selectors for syscall)
    // STAR[47:32] = Kernel CS (0x08)
    // STAR[63:48] = User CS base (0x18, +8 for user CS, +16 for user SS)
    uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x18 << 48);
    asm volatile("wrmsr" : : "c"(0xC0000081), "a"((uint32_t)star), "d"((uint32_t)(star >> 32)));
    
    // Set up LSTAR MSR (syscall entry point)
    uint64_t lstar = (uint64_t)syscall_entry;
    asm volatile("wrmsr" : : "c"(0xC0000082), "a"((uint32_t)lstar), "d"((uint32_t)(lstar >> 32)));
    
    // Set up SFMASK MSR (RFLAGS mask)
    uint64_t sfmask = 0x200; // Clear IF (interrupts) during syscall
    asm volatile("wrmsr" : : "c"(0xC0000084), "a"((uint32_t)sfmask), "d"(0));
    
    // Enable SYSCALL in EFER
    uint32_t efer_low, efer_high;
    asm volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(0xC0000080));
    efer_low |= 1; // Set SCE bit
    asm volatile("wrmsr" : : "c"(0xC0000080), "a"(efer_low), "d"(efer_high));
}

// Update the kernel stack used by syscall entry
void syscall_set_kernel_stack(uint64_t stack) {
    extern uint64_t syscall_kernel_rsp;
    syscall_kernel_rsp = stack;
}