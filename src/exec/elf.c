#include "exec/elf.h"
#include "core/process.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/heap.h"
#include "lib/string.h"
#include <stdint.h>

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
    
    // Check if 64-bit
    if (header->e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }
    
    // Check if little-endian
    if (header->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }
    
    return true;
}

// Structure to track allocated segments for cleanup
typedef struct {
    uint64_t phys_addr;
    uint64_t num_pages;
} segment_alloc_t;

#define MAX_SEGMENTS 32
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

process_t* elf_load(const char* name, void* elf_data, size_t size) {
    (void)size; // Mark as used to silence warning
    
    if (!elf_validate(elf_data)) {
        return NULL;
    }
    
    elf64_header_t* header = (elf64_header_t*)elf_data;
    
    // Reset segment tracking for this load
    segment_count = 0;
    
    // Create a new process
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) return NULL;
    
    memset(proc, 0, sizeof(process_t));
    
    // Set up process info
    static uint32_t next_pid = 100; // Start user processes at 100
    proc->pid = next_pid++;
    proc->state = PROCESS_READY;
    proc->is_usermode = true;
    proc->has_been_interrupted = false;
    
    // Copy name
    if (name) {
        int i = 0;
        while (name[i] && i < 63) {
            proc->name[i] = name[i];
            i++;
        }
        proc->name[i] = '\0';
    }
    
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

    // Allocate user stack (4 pages)
    uint64_t user_stack_phys = (uint64_t)pmm_alloc_pages(4);
    if (!user_stack_phys) {
        process_free_page_table(proc->page_table);
        pmm_free_pages((void*)stack_phys, 2);
        kfree(proc);
        return NULL;
    }

    // Map user stack to high address (0x800000000000)
    #define USER_STACK_TOP 0x800000000000  // 8TB mark
    uint64_t user_stack_base = USER_STACK_TOP - (4 * PAGE_SIZE);

    for (int i = 0; i < 4; i++) {
        uint64_t virt_page = user_stack_base + (i * PAGE_SIZE);
        uint64_t phys_page = user_stack_phys + (i * PAGE_SIZE);
        
        if (!vmm_map_page(proc->page_table, virt_page, phys_page,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) {
            // Cleanup on failure
            cleanup_segments();
            process_free_page_table(proc->page_table);
            pmm_free_pages((void*)user_stack_phys, 4);
            pmm_free_pages((void*)stack_phys, 2);
            kfree(proc);
            return NULL;
        }
    }

    proc->user_stack_top = USER_STACK_TOP;

    // Load program segments
    elf64_program_header_t* ph = (elf64_program_header_t*)((uint8_t*)elf_data + header->e_phoff);
    
    for (int i = 0; i < header->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            uint64_t vaddr = ph[i].p_vaddr;
            uint64_t memsz = ph[i].p_memsz;
            uint64_t filesz = ph[i].p_filesz;
            uint64_t offset = ph[i].p_offset;
            
            // Calculate how many pages we need
            uint64_t vaddr_aligned = PAGE_ALIGN_DOWN(vaddr);
            uint64_t vaddr_end = vaddr + memsz;
            uint64_t vaddr_end_aligned = PAGE_ALIGN_UP(vaddr_end);
            uint64_t total_size = vaddr_end_aligned - vaddr_aligned;
            uint64_t pages_needed = total_size / PAGE_SIZE;
            
            // Allocate physical pages
            uint64_t segment_phys = (uint64_t)pmm_alloc_pages(pages_needed);
            if (!segment_phys) {
                // Cleanup on failure
                cleanup_segments();
                process_free_page_table(proc->page_table);
                pmm_free_pages((void*)stack_phys, 2);
                pmm_free_pages((void*)user_stack_phys, 4);
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
                
                if (!vmm_map_page(pt, virt_page, phys_page,
                  PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) {
                    // Cleanup on failure
                    cleanup_segments();
                    process_free_page_table(proc->page_table);
                    pmm_free_pages((void*)stack_phys, 2);
                    pmm_free_pages((void*)user_stack_phys, 4);
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
    
    // Initialize heap (after loading all segments)
    uint64_t highest_addr = 0;
    ph = (elf64_program_header_t*)((uint8_t*)elf_data + header->e_phoff);
    
    for (int i = 0; i < header->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
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
    
    // Clear segment tracking since we succeeded
    segment_count = 0;
    
    uint64_t* stack = (uint64_t*)proc->stack_top;

    if (proc->is_usermode) {
        *(--stack) = (uint64_t)process_entry_usermode;
    } else {
        *(--stack) = (uint64_t)process_entry;
    }
    
    proc->context.rsp = (uint64_t)stack;
    proc->context.rbp = 0;
    proc->context.rbx = 0;
    proc->context.r12 = header->e_entry;  // Store entry for process_entry wrapper
    proc->context.r13 = 0;
    proc->context.r14 = 0;
    proc->context.r15 = 0;
    proc->context.rflags = 0x202;
    
    // Initialize interrupt_context with valid usermode state
    // This will be the state when the process first gets interrupted
    memset(&proc->interrupt_context, 0, sizeof(proc->interrupt_context));
    proc->interrupt_context.rip = header->e_entry;  // Entry point
    proc->interrupt_context.cs = 0x1B;              // User code segment (ring 3)
    proc->interrupt_context.rflags = 0x202;         // Interrupts enabled
    proc->interrupt_context.rsp = USER_STACK_TOP;   // User stack
    proc->interrupt_context.ss = 0x23;              // User data segment (ring 3)
    
    // Add to process list
    extern process_t* process_list_head;
    proc->next = process_list_head;
    process_list_head = proc;

    return proc;
}

static uint64_t write_strings_on_stack(page_table_t* pt, uint64_t* sp,
                                       int argc, const char** argv,
                                       uint64_t* out_argv_ptr) {
    // We’ll place strings first (top-down), then argv array, then argc.
    uint64_t stack = *sp;
    uint64_t argv_ptrs[32]; // support up to 32 for safety
    if (argc > 32) argc = 32;

    // write strings
    for (int i = argc - 1; i >= 0; --i) {
        const char* s = argv[i];
        size_t L = 0; while (s && s[L]) L++;
        stack -= (uint64_t)(L + 1);
        // align to 16
        stack &= ~0xFULL;

        // Copy bytes into mapped pages: we can temporarily translate via phys_to_virt
        // by locating physical page with vmm_get_physical and writing through HH.
        for (size_t off = 0; off < L + 1; ++off) {
            uint64_t va = stack + off;
            uint64_t pa = vmm_get_physical(pt, va & ~0xFFFULL);
            uint8_t*  hh = (uint8_t*)phys_to_virt((pa & ~0xFFFULL)) + (va & 0xFFFULL);
            hh[0] = (off < L) ? (uint8_t)s[off] : 0;
        }
        argv_ptrs[i] = stack;
    }

    // space for argv array (argc+1 pointers)
    stack -= (uint64_t)((argc + 1) * sizeof(uint64_t));
    stack &= ~0xFULL;

    // write argv pointers
    for (int i=0;i<argc;i++) {
        uint64_t va = stack + (uint64_t)(i*sizeof(uint64_t));
        uint64_t pa = vmm_get_physical(pt, va & ~0xFFFULL);
        uint64_t* hh = (uint64_t*)phys_to_virt(pa) + ((va & 0xFFFULL) / 8);
        *hh = argv_ptrs[i];
    }
    // argv[argc] = NULL
    {
        uint64_t va = stack + (uint64_t)(argc*sizeof(uint64_t));
        uint64_t pa = vmm_get_physical(pt, va & ~0xFFFULL);
        uint64_t* hh = (uint64_t*)phys_to_virt(pa) + ((va & 0xFFFULL) / 8);
        *hh = 0;
    }

    *out_argv_ptr = stack;

    // finally push argc (as 64-bit)
    stack -= 8;
    stack &= ~0xFULL;
    {
        uint64_t va = stack;
        uint64_t pa = vmm_get_physical(pt, va & ~0xFFFULL);
        uint64_t* hh = (uint64_t*)phys_to_virt(pa) + ((va & 0xFFFULL) / 8);
        *hh = (uint64_t)argc;
    }

    *sp = stack;
    return stack;
}

process_t* elf_load_with_args(const char* name, void* elf_data, size_t size,
                              int argc, const char** argv) {
    process_t* proc = elf_load(name, elf_data, size); // your existing loader
    if (!proc || !proc->page_table || !proc->is_usermode) return proc;

    // Seed stack with argc/argv per SysV AMD64 ABI: rdi=argc, rsi=argv
    uint64_t sp = proc->user_stack_top;
    uint64_t argv_va = 0;
    write_strings_on_stack(proc->page_table, &sp, argc, argv, &argv_va);

    // set initial interrupt frame (the first “start” path in your timer switcher uses these)
    proc->interrupt_context.rip    = ((elf64_header_t*)elf_data)->e_entry;
    proc->interrupt_context.cs     = 0x1B;         // user code seg
    proc->interrupt_context.rflags = 0x202;
    proc->interrupt_context.rsp    = sp;
    proc->interrupt_context.ss     = 0x23;         // user data seg
    proc->interrupt_context.rdi    = (uint64_t)argc;
    proc->interrupt_context.rsi    = (uint64_t)argv_va;

    return proc;
}

// optional: keep old API as wrapper
process_t* elf_load(const char* name, void* elf_data, size_t size);
