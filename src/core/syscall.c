#include "core/syscall.h"
#include "core/process.h"
#include "limine.h"
#include "arch/x86/tss.h"
#include "drivers/timer.h"
#include "tar.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include "drivers/acpi.h"
#include "lib/string.h"

#include <stdint.h>
#include <stdbool.h>

extern struct limine_framebuffer* fb0(void);
extern void print(struct limine_framebuffer* fb, const char* s);
extern void print_hex(struct limine_framebuffer* fb, uint64_t num);
extern char keyboard_getchar(void);
extern int keyboard_getchar_nonblocking(void);
extern void putc_fb(struct limine_framebuffer* fb, char c);

// IO port functions (copied from io.h since they're static inline there)
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Safe copy from kernel to user space
static void copy_to_user(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

// Validate that a pointer is in userspace range
static bool is_userspace_ptr(uint64_t ptr, size_t len) {
    uint64_t hhdm = hhdm_get_offset();
    if (ptr >= hhdm) {
        return false; // Kernel address
    }
    if (ptr + len < ptr) {
        return false; // Overflow
    }
    // Also check upper bound - user stack is below 0x800000000000
    if (ptr + len > 0x800000000000ULL) {
        return false;
    }
    return true;
}

// Validate and get string length (max 4096 bytes)
static bool validate_user_string(const char* str, size_t* out_len) {
    if (!is_userspace_ptr((uint64_t)str, 1)) {
        return false;
    }
    
    size_t len = 0;
    while (len < 4096) {
        if (!is_userspace_ptr((uint64_t)(str + len), 1)) {
            return false;
        }
        if (str[len] == '\0') {
            *out_len = len;
            return true;
        }
        len++;
    }
    return false;
}

// Full register frame
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} syscall_frame_t;

// File descriptor table (simple for now)
#define MAX_FDS 16
typedef struct {
    bool in_use;
    void* data;      // Pointer to file data in memory
    size_t size;     // File size
    size_t offset;   // Current read position
    char name[64];   // File name
    uint32_t owner_pid;  // ADD THIS
} fd_entry_t;

static fd_entry_t fd_table[MAX_FDS];

// Initialize file descriptor table
static void fd_table_init(void) {
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i].in_use = false;
    }
}

// Close all file descriptors for a process
static void fd_close_all_for_process(uint32_t pid) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_table[i].in_use && fd_table[i].owner_pid == pid) {
            fd_table[i].in_use = false;
        }
    }
}

void syscall_on_process_exit(uint32_t pid) {
    fd_close_all_for_process(pid);
}

// Allocate a file descriptor
static int fd_alloc(const char* name, void* data, size_t size) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fd_table[i].in_use) {
            fd_table[i].in_use = true;
            fd_table[i].data = data;
            fd_table[i].size = size;
            fd_table[i].offset = 0;
            fd_table[i].owner_pid = process_current() ? process_current()->pid : 0;
            
            // Copy name
            int j = 0;
            while (name[j] && j < 63) {
                fd_table[i].name[j] = name[j];
                j++;
            }
            fd_table[i].name[j] = '\0';
            
            return i;
        }
    }
    return -1; // No free FDs
}

// Syscall handler implementation
void syscall_handler_impl(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg2;
    (void)arg3;
    
    struct limine_framebuffer* fb = fb0();
    
    // Return value in RAX
    uint64_t retval = 0;
    
    switch(syscall_num) {
        case SYS_PRINT: {
            size_t len;
            if (arg1 && validate_user_string((const char*)arg1, &len)) {
                print(fb, (const char*)arg1);
                retval = len;
            } else {
                print(fb, "[invalid string pointer]\n");
                retval = (uint64_t)-1;
            }
            break;
        }

        case SYS_GETPID: {
            process_t* current = process_current();
            if (current) {
                retval = current->pid;
            }
            break;
        }

        case SYS_GETTIME: {
            retval = timer_get_ticks();
            break;
        }

        case SYS_YIELD: {
            // Just return - the timer will switch us eventually
            // For now, this is a no-op, but we could force a reschedule
            retval = 0;
            break;
        }

        case SYS_GETCHAR: {
            // Block until a character is available
            // For now, just call keyboard_getchar which blocks
            char c = keyboard_getchar();
            retval = (uint64_t)(unsigned char)c;
            break;
        }

        case SYS_GETCHAR_NONBLOCKING: {
            char c = keyboard_getchar_nonblocking();
            if (c == -1) {
                retval = (uint64_t)-1; // No char available
            } else {
                retval = (uint64_t)(unsigned char)c;
            }
            break;
        }

        case SYS_POLL: {
            // Check if keyboard data is available
            uint8_t status = inb(0x64);
            retval = (status & 0x01) ? 1 : 0;
            break;
        }

        

        case SYS_FB_INFO: {
            // arg1 = pointer to fb_info_t structure
            if (!is_userspace_ptr(arg1, sizeof(fb_info_t))) {
                retval = (uint64_t)-1;
                break;
            }
            
            fb_info_t* info = (fb_info_t*)arg1;
            if (fb) {
                info->address = (uint64_t)fb->address;
                info->width = fb->width;
                info->height = fb->height;
                info->pitch = fb->pitch;
                info->bpp = fb->bpp;
                retval = 0;
            } else {
                retval = (uint64_t)-1;
            }
            break;
        }

        case SYS_FB_MAP: {
            // Map framebuffer into process address space
            // arg1 = desired virtual address (or 0 for auto)
            // Returns: virtual address or -1
            
            if (!fb) {
                retval = (uint64_t)-1;
                break;
            }
            
            process_t* proc = process_current();
            if (!proc || !proc->page_table) {
                retval = (uint64_t)-1;
                break;
            }

            if (proc->fb_mapping_size != 0) {
                retval = proc->fb_mapping_virt_base;
                break;
            }
            
            // Calculate how many pages we need
            uint64_t fb_size = fb->pitch * fb->height;
            uint64_t pages_needed = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
            
            // Choose virtual address (simplified - use fixed address for now)
            uint64_t virt_addr = 0x600000000000ULL;  // 6TB mark
            
            // Get physical address of framebuffer
            // Limine gives us a virtual address in higher half, convert it
            uint64_t fb_virt = (uint64_t)fb->address;
            uint64_t fb_phys;
            
            // Check if it's already a higher-half address
            uint64_t hhdm = hhdm_get_offset();
            if (fb_virt >= hhdm) {
                // It's in the higher half, convert to physical
                fb_phys = fb_virt - hhdm;
            } else {
                // Assume it's already physical
                fb_phys = fb_virt;
            }
            
            // Map each page
            bool success = true;
            for (uint64_t i = 0; i < pages_needed; i++) {
                uint64_t virt_page = virt_addr + (i * PAGE_SIZE);
                uint64_t phys_page = fb_phys + (i * PAGE_SIZE);
                
                if (!vmm_map_page(proc->page_table, virt_page, phys_page,
                                PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) {
                    // Failed - unmap what we've done
                    for (uint64_t j = 0; j < i; j++) {
                        vmm_unmap_page(proc->page_table, virt_addr + (j * PAGE_SIZE));
                    }
                    success = false;
                    break;
                }
            }
            
            if (success) {
                uint64_t mapped_size = pages_needed * PAGE_SIZE;
                proc->fb_mapping_phys_base = fb_phys;
                proc->fb_mapping_size = mapped_size;
                proc->fb_mapping_virt_base = virt_addr;
                retval = virt_addr;
            } else {
                retval = (uint64_t)-1;
            }
            break;
        }

        case SYS_BRK: {
            // arg1 = new heap end address (or 0 to query current)
            // Returns: current/new heap end address
            
            process_t* proc = process_current();
            if (!proc) {
                retval = (uint64_t)-1;
                break;
            }
            
            // If arg1 is 0, just return current heap end
            if (arg1 == 0) {
                retval = proc->heap_end;
                break;
            }
            
            uint64_t new_end = arg1;
            
            // Validate new end is in userspace and above heap start
            uint64_t hhdm = hhdm_get_offset();
            if (new_end >= hhdm || new_end < proc->heap_start) {
                retval = proc->heap_end;  // Return old value on error
                break;
            }
            
            // Don't allow heap to grow past user stack (leave some space)
            uint64_t max_heap = 0x500000000000ULL;  // 5TB mark (well below 8TB stack)
            if (new_end > max_heap) {
                retval = proc->heap_end;
                break;
            }
            
            // Calculate which pages we need
            uint64_t old_end_page = PAGE_ALIGN_UP(proc->heap_end);
            uint64_t new_end_page = PAGE_ALIGN_UP(new_end);
            
            if (new_end_page > old_end_page) {
                // Growing heap - need to allocate and map new pages
                uint64_t pages_needed = (new_end_page - old_end_page) / PAGE_SIZE;
                bool grow_ok = true;
                uint64_t pages_allocated = 0;

                for (uint64_t i = 0; i < pages_needed; i++) {
                    uint64_t virt_addr = old_end_page + (i * PAGE_SIZE);
                    uint64_t phys_page = (uint64_t)pmm_alloc();

                    if (!phys_page) {
                        grow_ok = false;
                        break;
                    }

                    if (!vmm_map_page(proc->page_table, virt_addr, phys_page,
                                    PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) {
                        // Mapping failed - free the page and return
                        pmm_free((void*)phys_page);
                        grow_ok = false;
                        break;
                    }

                    // Zero out the new page
                    uint8_t* page_virt = (uint8_t*)phys_to_virt(phys_page);
                    memset(page_virt, 0, PAGE_SIZE);

                    pages_allocated++;
                }

                if (!grow_ok) {
                    for (uint64_t j = 0; j < pages_allocated; j++) {
                        uint64_t virt_addr = old_end_page + (j * PAGE_SIZE);
                        uint64_t phys = vmm_get_physical(proc->page_table, virt_addr);
                        if (phys) {
                            vmm_unmap_page(proc->page_table, virt_addr);
                            pmm_free((void*)phys);
                        }
                    }

                    retval = proc->heap_end;
                    break;
                }

                // Update heap end
                proc->heap_end = new_end;
                retval = new_end;

            } else if (new_end_page < old_end_page) {
                // Shrinking heap - unmap pages
                uint64_t pages_to_free = (old_end_page - new_end_page) / PAGE_SIZE;
                
                for (uint64_t i = 0; i < pages_to_free; i++) {
                    uint64_t virt_addr = new_end_page + (i * PAGE_SIZE);
                    uint64_t phys_addr = vmm_get_physical(proc->page_table, virt_addr);
                    
                    if (phys_addr) {
                        vmm_unmap_page(proc->page_table, virt_addr);
                        pmm_free((void*)phys_addr);
                    }
                }
                
                proc->heap_end = new_end;
                retval = new_end;
                
            } else {
                // No page boundary crossed, just update pointer
                proc->heap_end = new_end;
                retval = new_end;
            }
            
            break;
        }

        case SYS_EXIT: {
            process_t* current = process_current();
            
            if (current) {
                // Close all file descriptors for this process
                fd_close_all_for_process(current->pid);
                
                current->state = PROCESS_TERMINATED;
                print(fb, "\nProcess ");
                print(fb, current->name);
                print(fb, " exited with code ");
                print_hex(fb, arg1);
                print(fb, "\n");
            }
            
            process_cleanup_terminated();
            
            // Find another ready process
            process_t* next = process_get_list();
            while (next) {
                if (next != current && next->state == PROCESS_READY && next->pid != 0) {
                    // Found a ready process
                    if (next->has_been_interrupted) {
                        frame->r15 = next->interrupt_context.r15;
                        frame->r14 = next->interrupt_context.r14;
                        frame->r13 = next->interrupt_context.r13;
                        frame->r12 = next->interrupt_context.r12;
                        frame->r11 = next->interrupt_context.r11;
                        frame->r10 = next->interrupt_context.r10;
                        frame->r9  = next->interrupt_context.r9;
                        frame->r8  = next->interrupt_context.r8;
                        frame->rbp = next->interrupt_context.rbp;
                        frame->rdi = next->interrupt_context.rdi;
                        frame->rsi = next->interrupt_context.rsi;
                        frame->rdx = next->interrupt_context.rdx;
                        frame->rcx = next->interrupt_context.rcx;
                        frame->rbx = next->interrupt_context.rbx;
                        frame->rax = next->interrupt_context.rax;
                        frame->rip = next->interrupt_context.rip;
                        frame->cs  = next->interrupt_context.cs;
                        frame->rflags = next->interrupt_context.rflags;
                        frame->rsp = next->interrupt_context.rsp;
                        frame->ss  = next->interrupt_context.ss;
                    } else {
                        frame->r15 = 0;
                        frame->r14 = 0;
                        frame->r13 = 0;
                        frame->r12 = 0;
                        frame->r11 = 0;
                        frame->r10 = 0;
                        frame->r9  = 0;
                        frame->r8  = 0;
                        frame->rbp = 0;
                        frame->rdi = 0;
                        frame->rsi = 0;
                        frame->rdx = 0;
                        frame->rcx = 0;
                        frame->rbx = 0;
                        frame->rax = 0;
                        frame->rip = next->interrupt_context.rip;
                        frame->cs  = next->interrupt_context.cs;
                        frame->rflags = next->interrupt_context.rflags;
                        frame->rsp = next->interrupt_context.rsp;
                        frame->ss  = next->interrupt_context.ss;
                        next->has_been_interrupted = true;
                    }
                    
                    next->state = PROCESS_RUNNING;
                    tss_set_kernel_stack(next->stack_top);
                    
                    if (next->page_table) {
                        extern void vmm_switch_page_table(page_table_t* pt);
                        vmm_switch_page_table(next->page_table);
                    }
                    
                    extern void process_set_current(process_t* proc);
                    process_set_current(next);
                    return;
                }
                next = next->next;
            }
            
            // No other usermode processes
            print(fb, "All processes finished\n");
            
            next = process_get_list();
            while (next) {
                if (next->pid == 0) {
                    break;
                }
                next = next->next;
            }
            
            if (next && next->context.rsp != 0) {
                uint64_t* saved_stack = (uint64_t*)next->context.rsp;
                uint64_t return_addr = saved_stack[0];
                
                // **Make the iret frame to return to ring0 idle/shell**
                frame->rip = return_addr;
                frame->cs  = 0x08;
                frame->ss  = 0x10;
                frame->rflags = next->context.rflags;
                frame->rsp = next->context.rsp + 8;
                
                frame->rbp = next->context.rbp;
                frame->rbx = next->context.rbx;
                frame->r12 = next->context.r12;
                frame->r13 = next->context.r13;
                frame->r14 = next->context.r14;
                frame->r15 = next->context.r15;
                
                frame->rax = 0;
                frame->rcx = 0;
                frame->rdx = 0;
                frame->rsi = 0;
                frame->rdi = 0;
                frame->r8  = 0;
                frame->r9  = 0;
                frame->r10 = 0;
                frame->r11 = 0;

                // <<< added: be sure we're not still on the dead user's CR3
                vmm_switch_page_table(vmm_get_kernel_page_table());
                // <<< added (optional but nice): ensure correct kernel stack in TSS
                tss_set_kernel_stack(next->stack_top);

                process_set_current(next);
                return;
            }
            
            break;
        }

        case SYS_GETTICKS: {
            retval = timer_get_ticks();
            break;
        }

        case SYS_SLEEP_MS: {
            uint64_t ms = arg1;
            uint64_t start = timer_get_ticks();
            uint64_t freq = timer_get_frequency();
            uint64_t target = start + (ms * freq) / 1000;
            while (timer_get_ticks() < target) {
                // Enable interrupts and halt
                // The timer interrupt will wake us up
                asm volatile ("sti; hlt");
            }
            retval = 0;
            break;
        }

        case SYS_SLEEP_TICKS: {
            uint64_t ticks = arg1;
            uint64_t start = timer_get_ticks();
            uint64_t target = start + ticks;
            while (timer_get_ticks() < target) {
                // Enable interrupts and halt
                asm volatile ("sti; hlt");
            }
            retval = 0;
            break;
        }  

        case SYS_GETTICKS_DELTA: {
            process_t* current = process_current();
            if (current) {
                retval = timer_get_ticks() - current->start_ticks;
            } else {
                retval = 0;
            }
            break;
        }

        case SYS_RAND: {
            // Simple LFSR for random number generation
            static uint32_t lfsr = 0xACE1u;
            lfsr ^= lfsr << 13;
            lfsr ^= lfsr >> 17;
            lfsr ^= lfsr << 5;
            retval = (uint64_t)lfsr;
            break;
        }

        case SYS_REBOOT:
            acpi_reboot();            // no-return
            break;

        case SYS_SHUTDOWN:
            acpi_poweroff();          // no-return
            break;
            
        default:
            print(fb, "[invalid syscall number]\n");
            retval = (uint64_t)-1;
            break;
    }
    
    // Set return value in RAX
    frame->rax = retval;
}

// Low-level syscall handler
__attribute__((naked))
void syscall_handler(void) {
    asm volatile (
        "push %rax\n"
        "push %rbx\n"
        "push %rcx\n"
        "push %rdx\n"
        "push %rsi\n"
        "push %rdi\n"
        "push %rbp\n"
        "push %r8\n"
        "push %r9\n"
        "push %r10\n"
        "push %r11\n"
        "push %r12\n"
        "push %r13\n"
        "push %r14\n"
        "push %r15\n"
        
        // Syscall convention: RAX=num, RBX=arg1, RCX=arg2, RDX=arg3
        // C calling convention: RDI=arg1, RSI=arg2, RDX=arg3, RCX=arg4, R8=arg5
        "mov %rax, %rdi\n"  // syscall number -> arg1
        "mov %rbx, %rsi\n"  // arg1 -> arg2
        "mov %rcx, %r8\n"   // arg2 -> temp (r8)
        "mov %rdx, %rcx\n"  // arg3 -> arg4
        "mov %r8, %rdx\n"   // temp (arg2) -> arg3
        "mov %rsp, %r8\n"   // frame pointer -> arg5
        "call syscall_handler_impl\n"
        
        "pop %r15\n"
        "pop %r14\n"
        "pop %r13\n"
        "pop %r12\n"
        "pop %r11\n"
        "pop %r10\n"
        "pop %r9\n"
        "pop %r8\n"
        "pop %rbp\n"
        "pop %rdi\n"
        "pop %rsi\n"
        "pop %rdx\n"
        "pop %rcx\n"
        "pop %rbx\n"
        "pop %rax\n"
        "iretq\n"
    );
}

void syscall_init(void) {
    fd_table_init();
}