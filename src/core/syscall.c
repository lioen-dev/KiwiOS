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
#include "drivers/hda.h"
#include "lib/string.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define K_EINVAL 22
#define K_EFAULT 14
#define K_ENOMEM 12
#define K_EBADF   9

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

static void set_errno(process_t* proc, int err) {
    if (proc) {
        proc->last_errno = err;
    }
}

static void save_interrupt_context_from_frame(process_t* proc, syscall_frame_t* frame) {
    if (!proc || !frame) {
        return;
    }

    proc->interrupt_context.r15 = frame->r15;
    proc->interrupt_context.r14 = frame->r14;
    proc->interrupt_context.r13 = frame->r13;
    proc->interrupt_context.r12 = frame->r12;
    proc->interrupt_context.r11 = frame->r11;
    proc->interrupt_context.r10 = frame->r10;
    proc->interrupt_context.r9  = frame->r9;
    proc->interrupt_context.r8  = frame->r8;
    proc->interrupt_context.rbp = frame->rbp;
    proc->interrupt_context.rdi = frame->rdi;
    proc->interrupt_context.rsi = frame->rsi;
    proc->interrupt_context.rdx = frame->rdx;
    proc->interrupt_context.rcx = frame->rcx;
    proc->interrupt_context.rbx = frame->rbx;
    proc->interrupt_context.rax = frame->rax;
    proc->interrupt_context.rip = frame->rip;
    proc->interrupt_context.cs  = frame->cs;
    proc->interrupt_context.rflags = frame->rflags;
    proc->interrupt_context.rsp = frame->rsp;
    proc->interrupt_context.ss  = frame->ss;
}

// Convert milliseconds to timer ticks using 64-bit arithmetic; returns false on overflow
static bool ms_to_ticks(uint64_t ms, uint64_t freq, uint64_t* out_ticks) {
    if (!out_ticks || freq == 0) {
        return false;
    }

    if (ms != 0 && ms > UINT64_MAX / freq) {
        return false;
    }

    uint64_t product = ms * freq;
    *out_ticks = product / 1000;
    return true;
}

static bool switch_to_next_process(syscall_frame_t* frame, process_t* current) {
    process_cleanup_terminated();

    process_t* next = process_get_list();
    while (next) {
        // Only schedule other usermode processes from the syscall path.
        // (Kernel threads use the separate voluntary context switch path.)
        if (next != current && next->state == PROCESS_READY && next->pid != 0 && next->is_usermode) {
            // Always restore the full saved interrupt context.
            // For brand-new processes, elf_load/elf_load_with_args pre-seed
            // interrupt_context with sane defaults (including argc/argv in RDI/RSI).
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
            next->has_been_interrupted = true;

            next->state = PROCESS_RUNNING;
            tss_set_kernel_stack(next->stack_top);

            if (next->page_table) {
                extern void vmm_switch_page_table(page_table_t* pt);
                vmm_switch_page_table(next->page_table);
            }

            extern void process_set_current(process_t* proc);
            process_set_current(next);
            return true;
        }
        next = next->next;
    }

    // No ready user processes, fall back to idle if possible
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

        vmm_switch_page_table(vmm_get_kernel_page_table());
        tss_set_kernel_stack(next->stack_top);

        extern void process_set_current(process_t* proc);
        process_set_current(next);
        return true;
    }

    return false;
}

static bool request_sleep_until(syscall_frame_t* frame, uint64_t target_tick) {
    process_t* current = process_current();
    if (!current) {
        return false;
    }

    current->sleep_until = target_tick;
    current->state = PROCESS_SLEEPING;
    current->sleep_interrupted = false;

    save_interrupt_context_from_frame(current, frame);
    current->interrupt_context.rax = 0;
    current->has_been_interrupted = true;

    if (switch_to_next_process(frame, current)) {
        return true;
    }

    // If no other process could be scheduled, put the current one back
    current->state = PROCESS_RUNNING;
    return false;
}

static bool range_is_free(process_t* proc, uint64_t base, uint64_t pages) {
    if (!proc || !proc->page_table) {
        return false;
    }

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = base + (i * PAGE_SIZE);
        if (vmm_get_physical(proc->page_table, va) != 0) {
            return false;
        }
    }

    return true;
}

static uint64_t find_free_range(process_t* proc, uint64_t start, uint64_t pages) {
    if (!proc || !proc->page_table) {
        return 0;
    }

    uint64_t limit = 0x800000000000ULL;
    uint64_t length = pages * PAGE_SIZE;
    uint64_t cursor = start;

    if (length == 0 || cursor >= limit) {
        return 0;
    }

    while (cursor + length <= limit) {
        if (range_is_free(proc, cursor, pages)) {
            return cursor;
        }
        cursor += PAGE_SIZE;
    }

    return 0;
}

// Close all file descriptors for a process
static void fd_close_all_for_process(process_t* proc) {
    if (!proc || !proc->fds_initialized) {
        return;
    }

    for (size_t i = 0; i < PROCESS_MAX_FDS; i++) {
        proc->fd_table[i].in_use = false;
        proc->fd_table[i].data = NULL;
        proc->fd_table[i].size = 0;
        proc->fd_table[i].offset = 0;
        proc->fd_table[i].flags = 0;
        proc->fd_table[i].name[0] = '\0';
    }
}

void syscall_on_process_exit(process_t* proc) {
    fd_close_all_for_process(proc);
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

        case SYS_SLEEP: {
            uint64_t ms = arg1;
            uint64_t freq = timer_get_frequency();
            process_t* current = process_current();
            uint64_t ticks = 0;

            if (!current || !ms_to_ticks(ms, freq, &ticks)) {
                set_errno(current, K_EINVAL);
                retval = (uint64_t)-1;
                break;
            }

            uint64_t target = timer_get_ticks() + ticks;

            if (request_sleep_until(frame, target)) {
                return;
            }

            retval = 0;
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

        case SYS_HDA_WRITE_PCM: {
            const int16_t* user_samples = (const int16_t*)arg1;
            size_t frames = (size_t)arg2;
            size_t channels = HDA_output_channels();
            size_t frame_bytes = channels * sizeof(int16_t);
            uint64_t total_bytes = (uint64_t)frames * (uint64_t)frame_bytes;

            if (frames == 0 || total_bytes == 0 || total_bytes > SIZE_MAX) {
                retval = 0;
                break;
            }

            if (!is_userspace_ptr(arg1, (size_t)total_bytes)) {
                retval = (uint64_t)-1;
                break;
            }

            int16_t* tmp = (int16_t*)kmalloc((size_t)total_bytes);
            if (!tmp) {
                retval = (uint64_t)-1;
                break;
            }

            // Copy from userspace
            size_t samples_to_copy = total_bytes / sizeof(int16_t);
            for (size_t i = 0; i < samples_to_copy; ++i) {
                tmp[i] = user_samples[i];
            }

            retval = HDA_enqueue_interleaved_pcm(tmp, frames);
            kfree(tmp);
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

        case SYS_FB_FLIP: {
            // No double buffering yet; succeed so callers don't crash.
            retval = 0;
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
                set_errno(proc, K_EINVAL);
                retval = (uint64_t)-1;
                break;
            }

            // Don't allow heap to grow past user stack (leave some space)
            uint64_t max_heap = 0x500000000000ULL;  // 5TB mark (well below 8TB stack)
            if (new_end > max_heap) {
                set_errno(proc, K_ENOMEM);
                retval = (uint64_t)-1;
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

                    set_errno(proc, K_ENOMEM);
                    retval = (uint64_t)-1;
                    break;
                }

                // Update heap end
                proc->heap_end = new_end;
                retval = 0;

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
                retval = 0;

            } else {
                // No page boundary crossed, just update pointer
                proc->heap_end = new_end;
                retval = 0;
            }

            break;
        }

        case SYS_MMAP: {
            process_t* proc = process_current();
            uint64_t virt_addr = arg1;
            uint64_t length = arg2;
            uint64_t prot = arg3;
            uint64_t flags = frame->rsi;
            int fd = (int)frame->rdi;
            uint64_t offset = frame->r8;

            if (!proc || !proc->page_table || length == 0) {
                set_errno(proc, K_EINVAL);
                retval = (uint64_t)-1;
                break;
            }

            bool want_shared = (flags & MAP_SHARED) != 0;
            bool want_private = (flags & MAP_PRIVATE) != 0;
            bool map_fixed = (flags & MAP_FIXED) != 0;
            bool anonymous = (flags & MAP_ANONYMOUS) != 0;

            if (want_shared == want_private) {
                set_errno(proc, K_EINVAL);
                retval = (uint64_t)-1;
                break;
            }

            uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
            uint64_t aligned_length = pages * PAGE_SIZE;

            if (map_fixed) {
                if (virt_addr == 0 || (virt_addr & (PAGE_SIZE - 1))) {
                    set_errno(proc, K_EINVAL);
                    retval = (uint64_t)-1;
                    break;
                }
            }

            if (virt_addr != 0) {
                virt_addr &= ~(PAGE_SIZE - 1);
            }

            uint64_t search_base = PAGE_ALIGN_UP(proc->heap_end);
            if (search_base < 0x40000000000ULL) {
                search_base = 0x40000000000ULL;
            }

            if (!map_fixed) {
                if (virt_addr && !range_is_free(proc, virt_addr, pages)) {
                    virt_addr = find_free_range(proc, virt_addr + PAGE_SIZE, pages);
                }

                if (virt_addr == 0 || !range_is_free(proc, virt_addr, pages)) {
                    virt_addr = find_free_range(proc, search_base, pages);
                }

                if (virt_addr == 0) {
                    set_errno(proc, K_ENOMEM);
                    retval = (uint64_t)-1;
                    break;
                }
            } else {
                if (!range_is_free(proc, virt_addr, pages)) {
                    set_errno(proc, K_EINVAL);
                    retval = (uint64_t)-1;
                    break;
                }
            }

            if (!is_userspace_ptr(virt_addr, aligned_length)) {
                set_errno(proc, K_EFAULT);
                retval = (uint64_t)-1;
                break;
            }

            const uint8_t* file_bytes = NULL;
            size_t file_size = 0;

            if (!anonymous) {
                if (fd < 0 || (size_t)fd >= PROCESS_MAX_FDS || !proc->fd_table[fd].in_use) {
                    set_errno(proc, K_EBADF);
                    retval = (uint64_t)-1;
                    break;
                }

                fd_entry_t* entry = &proc->fd_table[fd];

                if (offset > entry->size) {
                    set_errno(proc, K_EINVAL);
                    retval = (uint64_t)-1;
                    break;
                }

                file_bytes = (const uint8_t*)entry->data;
                file_size = entry->size;
            }

            uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
            if (prot & PROT_WRITE) {
                page_flags |= PAGE_WRITE;
            }

            bool ok = true;
            uint64_t mapped = 0;

            for (uint64_t i = 0; i < pages; i++) {
                uint64_t va = virt_addr + (i * PAGE_SIZE);
                void* phys = pmm_alloc();

                if (!phys) {
                    set_errno(proc, K_ENOMEM);
                    ok = false;
                    break;
                }

                if (!vmm_map_page(proc->page_table, va, (uint64_t)phys, page_flags)) {
                    pmm_free(phys);
                    set_errno(proc, K_ENOMEM);
                    ok = false;
                    break;
                }

                uint8_t* pv = (uint8_t*)phys_to_virt((uint64_t)phys);
                size_t copied = 0;

                if (!anonymous && file_bytes) {
                    size_t page_offset = i * PAGE_SIZE;

                    if (offset + page_offset < file_size) {
                        size_t remaining = file_size - (offset + page_offset);
                        copied = remaining > PAGE_SIZE ? PAGE_SIZE : remaining;
                        memcpy(pv, file_bytes + offset + page_offset, copied);
                    }
                }

                if (copied < PAGE_SIZE) {
                    memset(pv + copied, 0, PAGE_SIZE - copied);
                }

                mapped++;
            }

            if (!ok) {
                for (uint64_t i = 0; i < mapped; i++) {
                    uint64_t va = virt_addr + (i * PAGE_SIZE);
                    uint64_t phys = vmm_get_physical(proc->page_table, va);
                    if (phys) {
                        vmm_unmap_page(proc->page_table, va);
                        pmm_free((void*)phys);
                    }
                }

                retval = (uint64_t)-1;

            } else {
                retval = virt_addr;
            }
            break;
        }

        case SYS_MUNMAP: {
            process_t* proc = process_current();
            uint64_t virt_addr = arg1;
            uint64_t length = arg2;

            if (!proc || !proc->page_table || length == 0) {
                retval = (uint64_t)-1;
                break;
            }

            if (!is_userspace_ptr(virt_addr, length)) {
                retval = (uint64_t)-1;
                break;
            }

            uint64_t base = virt_addr & ~(PAGE_SIZE - 1);
            uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;

            for (uint64_t i = 0; i < pages; i++) {
                uint64_t va = base + (i * PAGE_SIZE);
                uint64_t phys = vmm_get_physical(proc->page_table, va);

                if (phys) {
                    bool is_fb = proc->fb_mapping_size &&
                                 phys >= proc->fb_mapping_phys_base &&
                                 phys < proc->fb_mapping_phys_base + proc->fb_mapping_size;

                    vmm_unmap_page(proc->page_table, va);
                    if (!is_fb) {
                        pmm_free((void*)phys);
                    }
                }
            }

            retval = 0;
            break;
        }

        case SYS_EXIT: {
            process_t* current = process_current();
            
            if (current) {
                // Close all file descriptors for this process
                fd_close_all_for_process(current);
                
                current->state = PROCESS_TERMINATED;
                print(fb, "\nProcess ");
                print(fb, current->name);
                print(fb, " exited with code ");
                print_hex(fb, arg1);
                print(fb, "\n");
            }

            process_cleanup_terminated();

            if (switch_to_next_process(frame, current)) {
                return;
            }

            // No other usermode processes
            print(fb, "All processes finished\n");

            break;
        }

        case SYS_GETTICKS: {
            retval = timer_get_ticks();
            break;
        }

        case SYS_SLEEP_MS: {
            uint64_t ms = arg1;
            uint64_t freq = timer_get_frequency();
            process_t* current = process_current();
            uint64_t ticks = 0;

            if (!current || !ms_to_ticks(ms, freq, &ticks)) {
                set_errno(current, K_EINVAL);
                retval = (uint64_t)-1;
                break;
            }

            uint64_t target = timer_get_ticks() + ticks;

            if (request_sleep_until(frame, target)) {
                return;
            }

            retval = 0;
            break;
        }

        case SYS_SLEEP_TICKS: {
            uint64_t ticks = arg1;
            process_t* current = process_current();

            if (!current) {
                retval = (uint64_t)-1;
                break;
            }

            uint64_t target = timer_get_ticks() + ticks;

            if (request_sleep_until(frame, target)) {
                return;
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
    // Per-process resources are initialized as processes are created.
}