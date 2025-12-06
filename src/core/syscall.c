#include "core/syscall.h"
#include "core/process.h"
#include "core/scheduler.h"
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

static bool validate_user_buffer(uint64_t ptr, size_t len) {
    if (len == 0) {
        return false;
    }
    return is_userspace_ptr(ptr, len);
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

static bool copy_from_user(uint64_t user_ptr, void* dst, size_t len) {
    if (!dst || !validate_user_buffer(user_ptr, len)) {
        return false;
    }

    const uint8_t* src = (const uint8_t*)user_ptr;
    uint8_t* out = (uint8_t*)dst;
    for (size_t i = 0; i < len; ++i) {
        out[i] = src[i];
    }
    return true;
}

static bool copy_to_user(uint64_t user_ptr, const void* src, size_t len) {
    if (!src || !validate_user_buffer(user_ptr, len)) {
        return false;
    }

    const uint8_t* in = (const uint8_t*)src;
    uint8_t* dest = (uint8_t*)user_ptr;
    for (size_t i = 0; i < len; ++i) {
        dest[i] = in[i];
    }
    return true;
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

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} k_timespec_t;

typedef struct {
    uint64_t value;
    bool should_return;
} syscall_result_t;

typedef syscall_result_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, syscall_frame_t*);

#define SYSCALL_RETURN(val) ((syscall_result_t){ .value = (uint64_t)(val), .should_return = true })
#define SYSCALL_NO_RETURN   ((syscall_result_t){ .value = 0, .should_return = false })

// Copy a userspace timespec into kernel memory
static bool copy_user_timespec(uint64_t user_ptr, k_timespec_t* out) {
    if (!out || !user_ptr) {
        return false;
    }

    return copy_from_user(user_ptr, out, sizeof(k_timespec_t));
}

// Write a timespec back to userspace (best-effort)
static void write_user_timespec_zero(uint64_t user_ptr) {
    if (!user_ptr) {
        return;
    }

    k_timespec_t zero = {0};
    copy_to_user(user_ptr, &zero, sizeof(k_timespec_t));
}

// Convert a POSIX timespec into timer ticks (rounded up) with overflow checks
static bool timespec_to_ticks(const k_timespec_t* ts, uint64_t freq, uint64_t* out_ticks) {
    if (!ts || !out_ticks || freq == 0) {
        return false;
    }

    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000LL) {
        return false;
    }
    // Compute ticks = ceil((tv_sec * 1e9 + tv_nsec) * freq / 1e9)
    // Split the computation to avoid 128-bit division support requirements.
    if ((uint64_t)ts->tv_sec > UINT64_MAX / freq) {
        return false; // sec * freq would overflow
    }

    uint64_t sec_ticks = (uint64_t)ts->tv_sec * freq;

    // tv_nsec < 1e9, so tv_nsec * freq fits in 64 bits for typical timer freqs.
    uint64_t nsec_scaled = (uint64_t)ts->tv_nsec * freq;
    uint64_t nsec_ticks = (nsec_scaled + 1000000000ULL - 1) / 1000000000ULL; // ceil division

    if (UINT64_MAX - nsec_ticks < sec_ticks) {
        return false; // total would overflow
    }

    *out_ticks = sec_ticks + nsec_ticks;
    return true;
}

static bool ms_to_ticks(uint64_t ms, uint64_t freq, uint64_t* out_ticks) {
    if (!out_ticks) {
        return false;
    }

    k_timespec_t ts = {
        .tv_sec = (int64_t)(ms / 1000),
        .tv_nsec = (int64_t)((ms % 1000) * 1000000ULL),
    };

    return timespec_to_ticks(&ts, freq, out_ticks);
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
static syscall_result_t sys_print(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg2; (void)arg3; (void)frame;
    struct limine_framebuffer* fb = fb0();
    size_t len;
    if (arg1 && validate_user_string((const char*)arg1, &len)) {
        print(fb, (const char*)arg1);
        return SYSCALL_RETURN(len);
    }

    print(fb, "[invalid string pointer]\n");
    return SYSCALL_RETURN((uint64_t)-1);
}

static syscall_result_t sys_getpid(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    process_t* current = process_current();
    if (current) {
        return SYSCALL_RETURN(current->pid);
    }
    return SYSCALL_RETURN(0);
}

static syscall_result_t sys_gettime(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    return SYSCALL_RETURN(timer_get_ticks());
}

static syscall_result_t sys_sleep(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg3;
    process_t* current = process_current();
    uint64_t freq = timer_get_frequency();

    if (!current) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    k_timespec_t req;
    bool have_rem = arg2 != 0;

    if (!copy_user_timespec(arg1, &req)) {
        set_errno(current, K_EFAULT);
        return SYSCALL_RETURN((uint64_t)-1);
    }

    if (have_rem && !is_userspace_ptr(arg2, sizeof(k_timespec_t))) {
        set_errno(current, K_EFAULT);
        return SYSCALL_RETURN((uint64_t)-1);
    }

    uint64_t ticks = 0;
    if (!timespec_to_ticks(&req, freq, &ticks)) {
        set_errno(current, K_EINVAL);
        return SYSCALL_RETURN((uint64_t)-1);
    }

    if (ticks == 0) {
        if (have_rem) {
            write_user_timespec_zero(arg2);
        }
        return SYSCALL_RETURN(0);
    }

    uint64_t target = timer_get_ticks() + ticks;

    if (scheduler_sleep_until((uint64_t*)frame, target)) {
        if (have_rem) {
            write_user_timespec_zero(arg2);
        }
        return SYSCALL_NO_RETURN;
    }

    if (have_rem) {
        write_user_timespec_zero(arg2);
    }
    return SYSCALL_RETURN(0);
}

static syscall_result_t sys_yield(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    return SYSCALL_RETURN(0);
}

static syscall_result_t sys_getchar(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    char c = keyboard_getchar();
    return SYSCALL_RETURN((uint64_t)(unsigned char)c);
}

static syscall_result_t sys_getchar_nonblocking(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    char c = keyboard_getchar_nonblocking();
    if (c == -1) {
        return SYSCALL_RETURN((uint64_t)-1);
    }
    return SYSCALL_RETURN((uint64_t)(unsigned char)c);
}

static syscall_result_t sys_poll(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    uint8_t status = inb(0x64);
    return SYSCALL_RETURN((status & 0x01) ? 1 : 0);
}

static syscall_result_t sys_hda_write_pcm_handler(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg3; (void)frame;
    const int16_t* user_samples = (const int16_t*)arg1;
    size_t frames = (size_t)arg2;
    size_t channels = HDA_output_channels();
    size_t frame_bytes = channels * sizeof(int16_t);
    uint64_t total_bytes = (uint64_t)frames * (uint64_t)frame_bytes;

    if (frames == 0 || total_bytes == 0 || total_bytes > SIZE_MAX) {
        return SYSCALL_RETURN(0);
    }

    if (!is_userspace_ptr(arg1, (size_t)total_bytes)) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    int16_t* tmp = (int16_t*)kmalloc((size_t)total_bytes);
    if (!tmp) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    size_t samples_to_copy = total_bytes / sizeof(int16_t);
    for (size_t i = 0; i < samples_to_copy; ++i) {
        tmp[i] = user_samples[i];
    }

    uint64_t ret = HDA_enqueue_interleaved_pcm(tmp, frames);
    kfree(tmp);
    return SYSCALL_RETURN(ret);
}

static syscall_result_t sys_fb_info(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg2; (void)arg3; (void)frame;
    struct limine_framebuffer* fb = fb0();
    fb_info_t info = {0};

    if (!fb || !copy_from_user(arg1, &info, sizeof(fb_info_t))) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    info.address = (uint64_t)fb->address;
    info.width = fb->width;
    info.height = fb->height;
    info.pitch = fb->pitch;
    info.bpp = fb->bpp;

    if (!copy_to_user(arg1, &info, sizeof(fb_info_t))) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    return SYSCALL_RETURN(0);
}

static syscall_result_t sys_fb_map(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    struct limine_framebuffer* fb = fb0();
    if (!fb) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    process_t* proc = process_current();
    if (!proc || !proc->page_table) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    if (proc->fb_mapping_size != 0) {
        return SYSCALL_RETURN(proc->fb_mapping_virt_base);
    }

    uint64_t fb_size = fb->pitch * fb->height;
    uint64_t pages_needed = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t virt_addr = 0x600000000000ULL;

    uint64_t fb_virt = (uint64_t)fb->address;
    uint64_t fb_phys;

    uint64_t hhdm = hhdm_get_offset();
    if (fb_virt >= hhdm) {
        fb_phys = fb_virt - hhdm;
    } else {
        fb_phys = fb_virt;
    }

    bool success = true;
    for (uint64_t i = 0; i < pages_needed; i++) {
        uint64_t virt_page = virt_addr + (i * PAGE_SIZE);
        uint64_t phys_page = fb_phys + (i * PAGE_SIZE);

        if (!vmm_map_page(proc->page_table, virt_page, phys_page,
                          PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) {
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
        return SYSCALL_RETURN(virt_addr);
    }

    return SYSCALL_RETURN((uint64_t)-1);
}

static syscall_result_t sys_fb_flip(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    return SYSCALL_RETURN(0);
}

static syscall_result_t sys_brk(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg2; (void)arg3; (void)frame;
    process_t* proc = process_current();
    if (!proc) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    if (arg1 == 0) {
        return SYSCALL_RETURN(proc->heap_end);
    }

    uint64_t new_end = arg1;
    uint64_t hhdm = hhdm_get_offset();
    if (new_end >= hhdm || new_end < proc->heap_start) {
        set_errno(proc, K_EINVAL);
        return SYSCALL_RETURN((uint64_t)-1);
    }

    uint64_t max_heap = 0x500000000000ULL;
    if (new_end > max_heap) {
        set_errno(proc, K_ENOMEM);
        return SYSCALL_RETURN((uint64_t)-1);
    }

    uint64_t old_end_page = PAGE_ALIGN_UP(proc->heap_end);
    uint64_t new_end_page = PAGE_ALIGN_UP(new_end);

    if (new_end_page > old_end_page) {
        uint64_t pages_needed = (new_end_page - old_end_page) / PAGE_SIZE;
        uint64_t current_page = old_end_page;

        for (uint64_t i = 0; i < pages_needed; i++) {
            void* phys = pmm_alloc();
            if (!phys) {
                set_errno(proc, K_ENOMEM);
                return SYSCALL_RETURN((uint64_t)-1);
            }

            if (!vmm_map_page(proc->page_table, current_page, (uint64_t)phys,
                              PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) {
                pmm_free(phys);
                set_errno(proc, K_ENOMEM);
                return SYSCALL_RETURN((uint64_t)-1);
            }

            memset(phys_to_virt((uint64_t)phys), 0, PAGE_SIZE);
            current_page += PAGE_SIZE;
        }
    } else if (new_end_page < old_end_page) {
        uint64_t pages_to_free = (old_end_page - new_end_page) / PAGE_SIZE;
        uint64_t current_page = new_end_page;

        for (uint64_t i = 0; i < pages_to_free; i++) {
            uint64_t phys = vmm_get_physical(proc->page_table, current_page);
            if (phys) {
                vmm_unmap_page(proc->page_table, current_page);
                pmm_free((void*)phys);
            }
            current_page += PAGE_SIZE;
        }
    }

    proc->heap_end = new_end;
    return SYSCALL_RETURN(proc->heap_end);
}

static syscall_result_t sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)frame;
    process_t* proc = process_current();
    uint64_t length = arg2;
    int prot = (int)(arg3 & 0xFFFFFFFF);
    int flags = (int)(arg3 >> 32);

    if (!proc || !proc->page_table || length == 0) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t virt_addr = arg1;
    bool fixed = flags & MAP_FIXED;
    bool anonymous = flags & MAP_ANON;

    if (virt_addr == 0) {
        virt_addr = find_free_range(proc, 0x10000000000ULL, pages);
        if (virt_addr == 0) {
            return SYSCALL_RETURN((uint64_t)-1);
        }
    } else {
        if (!is_userspace_ptr(virt_addr, length)) {
            return SYSCALL_RETURN((uint64_t)-1);
        }
        virt_addr = PAGE_ALIGN_DOWN(virt_addr);
    }

    if (!fixed) {
        uint64_t candidate = find_free_range(proc, virt_addr, pages);
        if (candidate == 0) {
            return SYSCALL_RETURN((uint64_t)-1);
        }
        virt_addr = candidate;
    } else {
        if (!range_is_free(proc, virt_addr, pages)) {
            return SYSCALL_RETURN((uint64_t)-1);
        }
    }

    const uint8_t* file_bytes = NULL;
    size_t file_size = 0;
    size_t offset = 0;

    if (!anonymous) {
        int fd = (int)prot;

        if (fd < 0 || fd >= (int)PROCESS_MAX_FDS || !proc->fd_table[fd].in_use) {
            set_errno(proc, K_EBADF);
            return SYSCALL_RETURN((uint64_t)-1);
        }

        fd_entry_t* entry = &proc->fd_table[fd];

        if (offset > entry->size) {
            set_errno(proc, K_EINVAL);
            return SYSCALL_RETURN((uint64_t)-1);
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

        return SYSCALL_RETURN((uint64_t)-1);
    }

    return SYSCALL_RETURN(virt_addr);
}

static syscall_result_t sys_munmap(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg3; (void)frame;
    process_t* proc = process_current();
    uint64_t virt_addr = arg1;
    uint64_t length = arg2;

    if (!proc || !proc->page_table || length == 0) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    if (!is_userspace_ptr(virt_addr, length)) {
        return SYSCALL_RETURN((uint64_t)-1);
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

    return SYSCALL_RETURN(0);
}

static syscall_result_t sys_exit(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg2; (void)arg3;
    struct limine_framebuffer* fb = fb0();
    process_t* current = process_current();

    if (current) {
        fd_close_all_for_process(current);
        process_exit((int)arg1);
        print(fb, "\nProcess ");
        print(fb, current->name);
        print(fb, " exited with code ");
        print_hex(fb, arg1);
        print(fb, "\n");
    }

    process_cleanup_terminated();

    if (scheduler_reschedule((uint64_t*)frame)) {
        return SYSCALL_NO_RETURN;
    }

    print(fb, "All processes finished\n");

    process_t* idle = process_find_idle();
    if (idle && idle != current) {
        if (idle->state != PROCESS_READY && idle->state != PROCESS_RUNNING) {
            idle->state = PROCESS_READY;
        }

        process_switch_to(idle);
    }

    asm volatile("cli; hlt");
    return SYSCALL_NO_RETURN;
}

static syscall_result_t sys_getticks(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    return SYSCALL_RETURN(timer_get_ticks());
}

static syscall_result_t sys_sleep_ms(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg2; (void)arg3;
    uint64_t ms = arg1;
    uint64_t freq = timer_get_frequency();
    process_t* current = process_current();
    uint64_t ticks = 0;

    if (!current || !ms_to_ticks(ms, freq, &ticks)) {
        set_errno(current, K_EINVAL);
        return SYSCALL_RETURN((uint64_t)-1);
    }

    uint64_t target = timer_get_ticks() + ticks;

    if (scheduler_sleep_until((uint64_t*)frame, target)) {
        return SYSCALL_NO_RETURN;
    }

    return SYSCALL_RETURN(0);
}

static syscall_result_t sys_sleep_ticks(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg2; (void)arg3;
    uint64_t ticks = arg1;
    process_t* current = process_current();

    if (!current) {
        return SYSCALL_RETURN((uint64_t)-1);
    }

    uint64_t target = timer_get_ticks() + ticks;

    if (scheduler_sleep_until((uint64_t*)frame, target)) {
        return SYSCALL_NO_RETURN;
    }

    return SYSCALL_RETURN(0);
}

static syscall_result_t sys_getticks_delta(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    process_t* current = process_current();
    if (current) {
        return SYSCALL_RETURN(timer_get_ticks() - current->start_ticks);
    }
    return SYSCALL_RETURN(0);
}

static syscall_result_t sys_rand(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    static uint32_t lfsr = 0xACE1u;
    lfsr ^= lfsr << 13;
    lfsr ^= lfsr >> 17;
    lfsr ^= lfsr << 5;
    return SYSCALL_RETURN((uint64_t)lfsr);
}

static syscall_result_t sys_reboot(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    acpi_reboot();
    return SYSCALL_NO_RETURN;
}

static syscall_result_t sys_shutdown(uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    (void)arg1; (void)arg2; (void)arg3; (void)frame;
    acpi_poweroff();
    return SYSCALL_NO_RETURN;
}

typedef struct {
    const char* name;
    syscall_fn_t fn;
} syscall_entry_t;

#define SYSCALL_TABLE_SIZE 71

static const syscall_entry_t syscall_table[SYSCALL_TABLE_SIZE] = {
    [SYS_EXIT] = { "exit", sys_exit },
    [SYS_PRINT] = { "print", sys_print },
    [SYS_GETPID] = { "getpid", sys_getpid },
    [SYS_GETTIME] = { "gettime", sys_gettime },
    [SYS_SLEEP] = { "sleep", sys_sleep },
    [SYS_YIELD] = { "yield", sys_yield },
    [SYS_MMAP] = { "mmap", sys_mmap },
    [SYS_MUNMAP] = { "munmap", sys_munmap },
    [SYS_BRK] = { "brk", sys_brk },
    [SYS_GETCHAR] = { "getchar", sys_getchar },
    [SYS_GETCHAR_NONBLOCKING] = { "getchar_nonblocking", sys_getchar_nonblocking },
    [SYS_POLL] = { "poll", sys_poll },
    [SYS_FB_INFO] = { "fb_info", sys_fb_info },
    [SYS_FB_MAP] = { "fb_map", sys_fb_map },
    [SYS_FB_FLIP] = { "fb_flip", sys_fb_flip },
    [SYS_GETTICKS] = { "getticks", sys_getticks },
    [SYS_SLEEP_MS] = { "sleep_ms", sys_sleep_ms },
    [SYS_SLEEP_TICKS] = { "sleep_ticks", sys_sleep_ticks },
    [SYS_GETTICKS_DELTA] = { "getticks_delta", sys_getticks_delta },
    [SYS_RAND] = { "rand", sys_rand },
    [SYS_REBOOT] = { "reboot", sys_reboot },
    [SYS_SHUTDOWN] = { "shutdown", sys_shutdown },
    [SYS_HDA_WRITE_PCM] = { "hda_write_pcm", sys_hda_write_pcm_handler },
};

void syscall_handler_impl(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, syscall_frame_t* frame) {
    struct limine_framebuffer* fb = fb0();

    syscall_result_t result = { .value = (uint64_t)-1, .should_return = true };

    if (syscall_num < SYSCALL_TABLE_SIZE && syscall_table[syscall_num].fn) {
        result = syscall_table[syscall_num].fn(arg1, arg2, arg3, frame);
    } else {
        print(fb, "[invalid syscall number]\n");
    }

    if (result.should_return) {
        frame->rax = result.value;
    }
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
