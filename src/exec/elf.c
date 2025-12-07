#include "exec/elf.h"
#include "core/process.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/heap.h"
#include "lib/string.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

#define USER_STACK_TOP 0x800000000000ULL  // 8TB mark
#define USER_STACK_PAGES 5                // 1 guard + 4 usable
#define USER_STACK_USABLE_PAGES 4
#define MAX_ARGV_ENTRIES 128
#define MAX_SEGMENTS 32

bool elf_validate(void* elf_data) {
    if (!elf_data) return false;

    elf64_header_t* header = (elf64_header_t*)elf_data;

    // Check magic number
    if (header->e_ident[EI_MAG0] != ELFMAG0 ||
        header->e_ident[EI_MAG1] != ELFMAG1 ||
        header->e_ident[EI_MAG2] != ELFMAG2 ||
        header->e_ident[EI_MAG3] != ELFMAG3) {
        return false;
    }

    // Check if 64-bit, little-endian, and current version
    if (header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB ||
        header->e_ident[EI_VERSION] != EV_CURRENT) {
        return false;
    }

    // Basic type/machine/version validation
    if (!((header->e_type == ET_EXEC) || (header->e_type == ET_DYN))) {
        return false;
    }

    if (header->e_machine != EM_X86_64 || header->e_version != EV_CURRENT) {
        return false;
    }

    // Program header bounds and sizes
    if (header->e_ehsize < sizeof(elf64_header_t)) return false;
    if (header->e_phentsize != sizeof(elf64_program_header_t)) return false;
    if (header->e_phnum == 0 || header->e_phnum > MAX_SEGMENTS) return false;
    if (header->e_phoff < header->e_ehsize) return false;

    // Ensure program header table fits in file
    uint64_t ph_bytes = (uint64_t)header->e_phnum * header->e_phentsize;
    if (header->e_phoff > UINT64_MAX - ph_bytes) return false;

    return true;
}

// Structure to track allocated segments for cleanup
typedef struct {
    uint64_t phys_addr;
    uint64_t num_pages;
} segment_alloc_t;

static segment_alloc_t segment_allocs[MAX_SEGMENTS];
static int segment_count = 0;

static void track_segment(uint64_t phys, uint64_t pages) {
    if (segment_count < MAX_SEGMENTS) {
        segment_allocs[segment_count].phys_addr = phys;
        segment_allocs[segment_count].num_pages = pages;
        segment_count++;
    }
}

static void cleanup_segments(void) {
    for (int i = 0; i < segment_count; i++) {
        pmm_free_pages((void*)segment_allocs[i].phys_addr, 
                       segment_allocs[i].num_pages);
    }
    segment_count = 0;
}

static bool segment_within_user(uint64_t base, uint64_t length) {
    const uint64_t user_top = 0x800000000000ULL;
    if (length == 0) {
        return false;
    }

    if (base >= user_top) {
        return false;
    }

    if (base > UINT64_MAX - length) {
        return false;
    }

    uint64_t end = base + length;
    return end <= user_top && end >= base;
}

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_CLKTCK   17
#define AT_BASE     7
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_EXECFN   31

#define RANDOM_SEED_INIT 0xC0FFEEU
#define RANDOM_DATA_SIZE 16

static bool user_copy(page_table_t* pt, uint64_t dest, const void* src, size_t len) {
    size_t written = 0;

    while (written < len) {
        uint64_t va = dest + written;
        uint64_t page = va & ~0xFFFULL;
        uint64_t pa = vmm_get_physical(pt, page);
        if (!pa) {
            return false;
        }

        size_t offset = (size_t)(va & 0xFFFULL);
        size_t chunk = PAGE_SIZE - offset;
        if (chunk > len - written) {
            chunk = len - written;
        }

        uint8_t* hh = (uint8_t*)phys_to_virt(pa) + offset;
        memcpy(hh, (const uint8_t*)src + written, chunk);
        written += chunk;
    }

    return true;
}

static bool user_write_qword(page_table_t* pt, uint64_t dest, uint64_t value) {
    return user_copy(pt, dest, &value, sizeof(uint64_t));
}

static uint64_t count_total_string_bytes(int argc, const char** argv) {
    uint64_t total = 0;
    for (int i = 0; i < argc; i++) {
        const char* s = argv ? argv[i] : NULL;
        if (!s) continue;
        size_t len = 0;
        while (s[len]) { len++; }
        total += (uint64_t)len + 1; // include null terminator
    }
    return total;
}

static void fill_random_bytes(uint8_t* buffer, size_t length, uint32_t seed_extra) {
    static uint32_t lfsr = RANDOM_SEED_INIT;
    lfsr ^= seed_extra;
    for (size_t i = 0; i < length; i++) {
        lfsr ^= lfsr << 13;
        lfsr ^= lfsr >> 17;
        lfsr ^= lfsr << 5;
        buffer[i] = (uint8_t)(lfsr & 0xFF);
    }
}

static bool build_initial_stack(process_t* proc, const elf64_header_t* header,
                                int argc, const char** argv, uint64_t phdr_addr,
                                uint64_t entry_point, uint64_t base_addr,
                                const char* exec_name) {
    if (!proc || !proc->page_table) {
        return false;
    }

    if (argc > MAX_ARGV_ENTRIES) {
        argc = MAX_ARGV_ENTRIES;
    }

    const char* platform = "x86_64";
    size_t platform_len = 6; // strlen("x86_64")

    size_t execfn_len = 0;
    if (exec_name) {
        while (exec_name[execfn_len]) { execfn_len++; }
    }

    uint64_t string_bytes = count_total_string_bytes(argc, argv);
    if (execfn_len) {
        string_bytes += (uint64_t)execfn_len + 1;
    }

    string_bytes += (uint64_t)platform_len + 1;

    uint64_t padding = (16 - (string_bytes % 16)) % 16;
    string_bytes += padding + RANDOM_DATA_SIZE;

    uint64_t strings_base = USER_STACK_TOP - string_bytes;

    // Auxiliary vector entries
    uint64_t auxv[][2] = {
        { AT_PHDR,     phdr_addr },
        { AT_PHENT,    header->e_phentsize },
        { AT_PHNUM,    header->e_phnum },
        { AT_ENTRY,    entry_point },
        { AT_PAGESZ,   PAGE_SIZE },
        { AT_BASE,     base_addr },
        { AT_UID,      0 },
        { AT_EUID,     0 },
        { AT_GID,      0 },
        { AT_EGID,     0 },
        { AT_SECURE,   0 },
        { AT_CLKTCK,   100 },
        { AT_PLATFORM, 0 },
        { AT_EXECFN,   0 },
        { AT_RANDOM,   0 },
        { AT_NULL,     0 },
    };

    const uint64_t aux_count = sizeof(auxv) / sizeof(auxv[0]);
    uint64_t array_qwords = 1                           // argc
                          + (uint64_t)argc + 1          // argv pointers + NULL
                          + 1                           // envp NULL
                          + (aux_count * 2);            // auxv key/value pairs

    uint64_t array_bytes = array_qwords * sizeof(uint64_t);

    uint64_t stack_start = strings_base - array_bytes;
    stack_start &= ~0xFULL; // 16-byte alignment

    uint64_t user_stack_base = USER_STACK_TOP - (USER_STACK_USABLE_PAGES * PAGE_SIZE);
    if (stack_start < user_stack_base) {
        return false; // Not enough stack space
    }

    // Copy argv strings contiguously starting at strings_base
    uint64_t cursor = strings_base;
    uint64_t argv_addrs[MAX_ARGV_ENTRIES];

    for (int i = 0; i < argc; i++) {
        const char* s = argv ? argv[i] : NULL;
        if (!s) {
            argv_addrs[i] = 0;
            continue;
        }

        size_t len = 0;
        while (s[len]) { len++; }

        if (!user_copy(proc->page_table, cursor, s, len + 1)) {
            return false;
        }

        argv_addrs[i] = cursor;
        cursor += (uint64_t)len + 1;
    }

    uint64_t execfn_addr = 0;
    if (execfn_len) {
        if (!user_copy(proc->page_table, cursor, exec_name, execfn_len + 1)) {
            return false;
        }
        execfn_addr = cursor;
        cursor += (uint64_t)execfn_len + 1;
    }

    uint64_t platform_addr = cursor;
    if (!user_copy(proc->page_table, cursor, platform, platform_len + 1)) {
        return false;
    }
    cursor += (uint64_t)platform_len + 1;

    // Align for AT_RANDOM
    if (padding > 0) {
        uint8_t zeros[16] = {0};
        if (!user_copy(proc->page_table, cursor, zeros, padding)) {
            return false;
        }
        cursor += padding;
    }

    uint64_t random_addr = cursor;
    uint8_t random_buf[RANDOM_DATA_SIZE];
    fill_random_bytes(random_buf, RANDOM_DATA_SIZE, (uint32_t)proc->pid ^ (uint32_t)header->e_entry);
    if (!user_copy(proc->page_table, random_addr, random_buf, RANDOM_DATA_SIZE)) {
        return false;
    }
    cursor += RANDOM_DATA_SIZE;

    (void)cursor; // silence unused warning in case of future changes

    // Populate aux entries with resolved addresses
    for (uint64_t i = 0; i < aux_count; i++) {
        if (auxv[i][0] == AT_PLATFORM) auxv[i][1] = platform_addr;
        if (auxv[i][0] == AT_EXECFN)  auxv[i][1] = execfn_addr;
        if (auxv[i][0] == AT_RANDOM)  auxv[i][1] = random_addr;
    }

    // Write argc/argv/envp/auxv
    uint64_t pos = stack_start;
    if (!user_write_qword(proc->page_table, pos, (uint64_t)argc)) return false;
    pos += 8;

    for (int i = 0; i < argc; i++) {
        if (!user_write_qword(proc->page_table, pos, argv_addrs[i])) return false;
        pos += 8;
    }

    if (!user_write_qword(proc->page_table, pos, 0)) return false; // argv NULL
    pos += 8;

    // envp NULL only
    if (!user_write_qword(proc->page_table, pos, 0)) return false;
    pos += 8;

    for (uint64_t i = 0; i < aux_count; i++) {
        if (!user_write_qword(proc->page_table, pos, auxv[i][0])) return false;
        pos += 8;
        if (!user_write_qword(proc->page_table, pos, auxv[i][1])) return false;
        pos += 8;
    }

    proc->interrupt_context.rsp = stack_start;
    proc->interrupt_context.rdi = (uint64_t)argc;
    proc->interrupt_context.rsi = stack_start + 8; // argv begins after argc
    proc->interrupt_context.rdx = stack_start + 8 + ((uint64_t)argc + 1) * 8; // envp

    return true;
}

static process_t* elf_load_internal(const char* name, void* elf_data, size_t size,
                                    int argc, const char** argv) {

    if (!elf_validate(elf_data)) {
        return NULL;
    }
    
    elf64_header_t* header = (elf64_header_t*)elf_data;
    
    uint64_t ph_bytes = (uint64_t)header->e_phnum * header->e_phentsize;
    if (header->e_phoff > size || ph_bytes > size - header->e_phoff) {
        return NULL;
    }

    elf64_program_header_t* ph = (elf64_program_header_t*)((uint8_t*)elf_data + header->e_phoff);

    bool has_load = false;
    bool has_interp = false;
    bool has_tls = false;
    bool has_phdr = false;
    uint64_t phdr_vaddr = 0;

    for (int i = 0; i < header->e_phnum; i++) {
        switch (ph[i].p_type) {
            case PT_LOAD:
                has_load = true;
                break;
            case PT_INTERP:
                has_interp = true;
                break;
            case PT_TLS:
                has_tls = true;
                break;
            case PT_PHDR:
                has_phdr = true;
                phdr_vaddr = ph[i].p_vaddr;
                break;
            default:
                break;
        }
    }

    if (!has_load || has_interp || has_tls) {
        return NULL; // Require at least one loadable segment and no unsupported interpreters/TLS yet
    }

    // Reset segment tracking for this load
    segment_count = 0;

    // Create a new process
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) return NULL;

    process_init_common(proc, name, process_alloc_pid(), true, process_current());

    // Allocate kernel stack
    uint64_t stack_phys = (uint64_t)pmm_alloc_pages(2);
    if (!stack_phys) {
        kfree(proc);
        return NULL;
    }
    uint64_t stack_virt = (uint64_t)phys_to_virt(stack_phys);
    proc->stack_top = stack_virt + (2 * PAGE_SIZE);  // Kernel stack

    // Create per-process page table
    proc->page_table = vmm_create_page_table();
    if (!proc->page_table) {
        pmm_free_pages((void*)stack_phys, 2);
        kfree(proc);
        return NULL;
    }

    // Allocate user stack (guard + usable pages)
    uint64_t user_stack_phys = (uint64_t)pmm_alloc_pages(USER_STACK_PAGES);
    if (!user_stack_phys) {
        process_free_page_table(proc->page_table);
        pmm_free_pages((void*)stack_phys, 2);
        kfree(proc);
        return NULL;
    }

    // Map user stack to high address (0x800000000000), leaving the lowest
    // page unmapped as a guard.
    uint64_t user_stack_base = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);

    for (int i = 1; i < USER_STACK_PAGES; i++) {
        uint64_t virt_page = user_stack_base + (i * PAGE_SIZE);
        uint64_t phys_page = user_stack_phys + (i * PAGE_SIZE);

        if (!vmm_map_page(proc->page_table, virt_page, phys_page,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) {
            // Cleanup on failure
            cleanup_segments();
            process_free_page_table(proc->page_table);
            pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
            pmm_free_pages((void*)stack_phys, 2);
            kfree(proc);
            return NULL;
        }
    }

    proc->user_stack_top = USER_STACK_TOP;

    // Load program segments
    uint64_t load_base = UINT64_MAX;

    for (int i = 0; i < header->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            uint64_t vaddr = ph[i].p_vaddr;
            uint64_t memsz = ph[i].p_memsz;
            uint64_t filesz = ph[i].p_filesz;
            uint64_t offset = ph[i].p_offset;

            if (offset > size || filesz > size - offset) {
                cleanup_segments();
                process_free_page_table(proc->page_table);
                pmm_free_pages((void*)stack_phys, 2);
                pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
                kfree(proc);
                return NULL;
            }

            if (filesz > memsz || memsz == 0) {
                cleanup_segments();
                process_free_page_table(proc->page_table);
                pmm_free_pages((void*)stack_phys, 2);
                pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
                kfree(proc);
                return NULL;
            }

            if (!segment_within_user(vaddr, memsz)) {
                cleanup_segments();
                process_free_page_table(proc->page_table);
                pmm_free_pages((void*)stack_phys, 2);
                pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
                kfree(proc);
                return NULL;
            }

            if (vaddr > UINT64_MAX - memsz) {
                cleanup_segments();
                process_free_page_table(proc->page_table);
                pmm_free_pages((void*)stack_phys, 2);
                pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
                kfree(proc);
                return NULL;
            }

            if (ph[i].p_align && (ph[i].p_align & (ph[i].p_align - 1))) {
                cleanup_segments();
                process_free_page_table(proc->page_table);
                pmm_free_pages((void*)stack_phys, 2);
                pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
                kfree(proc);
                return NULL;
            }

            if (ph[i].p_align && ((ph[i].p_vaddr % ph[i].p_align) != (ph[i].p_offset % ph[i].p_align))) {
                cleanup_segments();
                process_free_page_table(proc->page_table);
                pmm_free_pages((void*)stack_phys, 2);
                pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
                kfree(proc);
                return NULL;
            }

            // Calculate how many pages we need
            uint64_t vaddr_aligned = PAGE_ALIGN_DOWN(vaddr);
            uint64_t vaddr_end = vaddr + memsz;
            uint64_t vaddr_end_aligned = PAGE_ALIGN_UP(vaddr_end);
            uint64_t total_size = vaddr_end_aligned - vaddr_aligned;
            uint64_t pages_needed = total_size / PAGE_SIZE;

            if (vaddr_aligned < load_base) {
                load_base = vaddr_aligned;
            }

            uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
            if (ph[i].p_flags & PF_W) {
                page_flags |= PAGE_WRITE;
            }
            if (!(ph[i].p_flags & PF_X)) {
                page_flags |= PAGE_NO_EXEC;
            }

            // Allocate physical pages
            uint64_t segment_phys = (uint64_t)pmm_alloc_pages(pages_needed);
            if (!segment_phys) {
                // Cleanup on failure
                cleanup_segments();
                process_free_page_table(proc->page_table);
                pmm_free_pages((void*)stack_phys, 2);
                pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
                kfree(proc);
                return NULL;
            }

            // Track this segment for cleanup if needed
            track_segment(segment_phys, pages_needed);

            // Map pages to the requested virtual address
            page_table_t* pt = proc->page_table;

            for (uint64_t j = 0; j < pages_needed; j++) {
                uint64_t virt_page = vaddr_aligned + (j * PAGE_SIZE);
                uint64_t phys_page = segment_phys + (j * PAGE_SIZE);

                if (!vmm_map_page(pt, virt_page, phys_page, page_flags)) {
                    // Cleanup on failure
                    cleanup_segments();
                    process_free_page_table(proc->page_table);
                    pmm_free_pages((void*)stack_phys, 2);
                    pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
                    kfree(proc);
                    return NULL;
                }
            }

            // Write to the physical pages directly through higher half mapping
            // Zero all pages first
            for (uint64_t j = 0; j < pages_needed; j++) {
                uint64_t phys_page = segment_phys + (j * PAGE_SIZE);
                void* phys_virt = phys_to_virt(phys_page);
                memset(phys_virt, 0, PAGE_SIZE);
            }

            // Now copy the file data
            // Calculate which page and offset the vaddr starts at
            uint64_t offset_in_segment = vaddr - vaddr_aligned;
            uint64_t byte_pos = 0;

            while (byte_pos < filesz) {
                uint64_t page_index = (offset_in_segment + byte_pos) / PAGE_SIZE;
                uint64_t page_offset = (offset_in_segment + byte_pos) % PAGE_SIZE;
                uint64_t bytes_in_page = PAGE_SIZE - page_offset;
                if (bytes_in_page > filesz - byte_pos) {
                    bytes_in_page = filesz - byte_pos;
                }

                uint64_t phys_page = segment_phys + (page_index * PAGE_SIZE);
                void* dest = (void*)((uint8_t*)phys_to_virt(phys_page) + page_offset);
                memcpy(dest, (uint8_t*)elf_data + offset + byte_pos, bytes_in_page);

                byte_pos += bytes_in_page;
            }
        }
    }

    if (load_base == UINT64_MAX) {
        cleanup_segments();
        process_free_page_table(proc->page_table);
        pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
        pmm_free_pages((void*)stack_phys, 2);
        kfree(proc);
        return NULL;
    }

    uint64_t entry_point = header->e_entry;
    if (header->e_type == ET_DYN) {
        if (entry_point > UINT64_MAX - load_base) {
            cleanup_segments();
            process_free_page_table(proc->page_table);
            pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
            pmm_free_pages((void*)stack_phys, 2);
            kfree(proc);
            return NULL;
        }
        entry_point += load_base;
    }

    if (!segment_within_user(entry_point, 1)) {
        cleanup_segments();
        process_free_page_table(proc->page_table);
        pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
        pmm_free_pages((void*)stack_phys, 2);
        kfree(proc);
        return NULL;
    }

    uint64_t phdr_runtime = 0;
    if (has_phdr) {
        uint64_t bias = (header->e_type == ET_DYN) ? load_base : 0;
        if (phdr_vaddr > UINT64_MAX - bias) {
            cleanup_segments();
            process_free_page_table(proc->page_table);
            pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
            pmm_free_pages((void*)stack_phys, 2);
            kfree(proc);
            return NULL;
        }
        phdr_runtime = phdr_vaddr + bias;
    } else {
        if (header->e_phoff > UINT64_MAX - load_base) {
            cleanup_segments();
            process_free_page_table(proc->page_table);
            pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
            pmm_free_pages((void*)stack_phys, 2);
            kfree(proc);
            return NULL;
        }
        phdr_runtime = load_base + header->e_phoff;
    }

    uint64_t aux_base = (header->e_type == ET_DYN) ? load_base : 0;

    if (!segment_within_user(phdr_runtime, ph_bytes)) {
        cleanup_segments();
        process_free_page_table(proc->page_table);
        pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
        pmm_free_pages((void*)stack_phys, 2);
        kfree(proc);
        return NULL;
    }

    // Initialize heap (after loading all segments)
    uint64_t highest_addr = 0;
    ph = (elf64_program_header_t*)((uint8_t*)elf_data + header->e_phoff);

    for (int i = 0; i < header->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr > UINT64_MAX - ph[i].p_memsz) {
                cleanup_segments();
                process_free_page_table(proc->page_table);
                pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
                pmm_free_pages((void*)stack_phys, 2);
                kfree(proc);
                return NULL;
            }

            uint64_t seg_end = ph[i].p_vaddr + ph[i].p_memsz;
            if (seg_end > highest_addr) {
                highest_addr = seg_end;
            }
        }
    }

    // Heap starts at the next page boundary after the highest segment
    if (highest_addr > 0) {
        proc->heap_start = PAGE_ALIGN_UP(highest_addr);
    } else {
        proc->heap_start = 0x401000;  // Fallback: start after typical code segment
    }
    proc->heap_end = proc->heap_start;

    uint64_t* stack = (uint64_t*)proc->stack_top;

    if (proc->is_usermode) {
        *(--stack) = (uint64_t)process_entry_usermode;
    } else {
        *(--stack) = (uint64_t)process_entry;
    }

    proc->context.rsp = (uint64_t)stack;
    proc->context.rbp = 0;
    proc->context.rbx = 0;
    proc->context.r12 = entry_point;  // Store entry for process_entry wrapper
    proc->context.r13 = 0;
    proc->context.r14 = 0;
    proc->context.r15 = 0;
    proc->context.rflags = 0x202;

    // Initialize interrupt_context with valid usermode state
    memset(&proc->interrupt_context, 0, sizeof(proc->interrupt_context));
    proc->interrupt_context.rip = entry_point;  // Entry point
    proc->interrupt_context.cs = 0x1B;              // User code segment (ring 3)
    proc->interrupt_context.rflags = 0x202;         // Interrupts enabled
    proc->interrupt_context.ss = 0x23;              // User data segment (ring 3)

    if (!build_initial_stack(proc, header, argc, argv, phdr_runtime,
                             entry_point, aux_base, name)) {
        cleanup_segments();
        process_free_page_table(proc->page_table);
        pmm_free_pages((void*)user_stack_phys, USER_STACK_PAGES);
        pmm_free_pages((void*)stack_phys, 2);
        kfree(proc);
        return NULL;
    }

    proc->uses_linux_abi = true;

    // Clear segment tracking since we succeeded
    segment_count = 0;

    // Add to process list
    process_register(proc);

    return proc;
}

process_t* elf_load_with_args(const char* name, void* elf_data, size_t size,
                              int argc, const char** argv) {
    return elf_load_internal(name, elf_data, size, argc, argv);
}

process_t* elf_load(const char* name, void* elf_data, size_t size) {
    return elf_load_internal(name, elf_data, size, 0, NULL);
}
