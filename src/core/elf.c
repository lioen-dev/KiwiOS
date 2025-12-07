#include "core/elf.h"
#include "core/process.h"
#include "fs/ext2.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "memory/pmm.h"
#include "lib/string.h"

extern ext2_fs_t* g_fs; // From main.c

bool elf_validate(const elf64_ehdr_t* ehdr) {
    if (!ehdr) return false;
    
    // Check magic number
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return false;
    }
    
    // Check class (64-bit)
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }
    
    // Check endianness (little endian)
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }
    
    // Check machine type (x86-64)
    if (ehdr->e_machine != EM_X86_64) {
        return false;
    }
    
    // Check type (executable or dynamic)
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        return false;
    }
    
    return true;
}

// Convert ELF permissions to page flags
static uint64_t elf_prot_to_flags(uint32_t p_flags) {
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    
    if (p_flags & PF_W) {
        flags |= PAGE_WRITE;
    }
    
    if (!(p_flags & PF_X)) {
        flags |= PAGE_NO_EXEC;
    }
    
    return flags;
}

// Load a PT_LOAD segment into process memory
static bool load_segment(process_t* proc, const elf64_phdr_t* phdr, const void* file_data) {
    if (phdr->p_type != PT_LOAD) return true;
    
    uint64_t vaddr = phdr->p_vaddr;
    uint64_t memsz = phdr->p_memsz;
    uint64_t filesz = phdr->p_filesz;
    uint64_t offset = phdr->p_offset;
    uint64_t flags = elf_prot_to_flags(phdr->p_flags);
    
    // Align to page boundaries
    uint64_t vaddr_aligned = PAGE_ALIGN_DOWN(vaddr);
    uint64_t vaddr_end = PAGE_ALIGN_UP(vaddr + memsz);
    
    // Allocate and map pages
    for (uint64_t va = vaddr_aligned; va < vaddr_end; va += PAGE_SIZE) {
        void* phys_page = pmm_alloc();
        if (!phys_page) {
            return false;
        }
        
        // Zero the page first
        uint8_t* page_virt = (uint8_t*)phys_to_virt((uint64_t)phys_page);
        memset(page_virt, 0, PAGE_SIZE);
        
        // Calculate what data to copy
        if (va >= vaddr && va < vaddr + filesz) {
            uint64_t page_offset = va - vaddr;
            uint64_t file_offset = offset + page_offset;
            uint64_t copy_size = PAGE_SIZE;
            
            if (va + PAGE_SIZE > vaddr + filesz) {
                copy_size = (vaddr + filesz) - va;
            }
            
            // Copy data from file
            memcpy(page_virt, (const uint8_t*)file_data + file_offset, copy_size);
        }
        
        // Map into process address space
        if (!vmm_map_page(proc->page_table, va, (uint64_t)phys_page, flags)) {
            pmm_free(phys_page);
            return false;
        }
    }
    
    return true;
}

// Setup user stack with argc, argv, envp, and auxiliary vector
static bool setup_stack(process_t* proc, char** argv, char** envp, 
                       uint64_t phdr_addr, uint64_t phent, uint64_t phnum,
                       uint64_t entry) {
    (void)argv; (void)envp; (void)phdr_addr; (void)phent; (void)phnum; (void)entry; // TODO: Implement full stack setup
    
    // Allocate stack (8MB at 0x7FFFFFFFE000)
    const uint64_t stack_top = 0x7FFFFFFFE000;
    const uint64_t stack_size = 8 * 1024 * 1024; // 8 MB
    const uint64_t stack_bottom = stack_top - stack_size;
    
    // Allocate stack pages
    for (uint64_t va = stack_bottom; va < stack_top; va += PAGE_SIZE) {
        void* phys = pmm_alloc();
        if (!phys) return false;
        
        memset(phys_to_virt((uint64_t)phys), 0, PAGE_SIZE);
        
        if (!vmm_map_page(proc->page_table, va, (uint64_t)phys, 
                         PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NO_EXEC)) {
            pmm_free(phys);
            return false;
        }
    }
    
    // Count arguments and environment variables
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    
    int envc = 0;
    if (envp) {
        while (envp[envc]) envc++;
    }
    
    // Build stack contents (we'll do this in a temporary buffer then copy)
    // Stack layout (growing down):
    // - auxiliary vector (terminated with AT_NULL)
    // - NULL
    // - envp strings
    // - NULL  
    // - argv strings
    // - envp[n] pointers
    // - argv[n] pointers
    // - argc
    
    // For simplicity, we'll use the last page of the stack as a build area
    uint64_t build_page_va = stack_top - PAGE_SIZE;
    uint8_t* build_area = (uint8_t*)phys_to_virt(vmm_get_physical(proc->page_table, build_page_va));
    memset(build_area, 0, PAGE_SIZE);
    
    proc->user_stack = stack_top - 16; // Leave some space
    
    return true;
}

process_t* elf_load(const char* path, char** argv, char** envp) {
    if (!g_fs || !path) return NULL;
    
    // Read the ELF file
    size_t file_size = 0;
    void* file_data = ext2_read_entire_file(g_fs, path, &file_size);
    if (!file_data) {
        return NULL;
    }
    
    // Validate ELF header
    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)file_data;
    if (!elf_validate(ehdr)) {
        kfree(file_data);
        return NULL;
    }
    
    // Create new process
    process_t* proc = process_create(path, false);
    if (!proc) {
        kfree(file_data);
        return NULL;
    }
    
    // Load program headers
    elf64_phdr_t* phdrs = (elf64_phdr_t*)((uint8_t*)file_data + ehdr->e_phoff);
    
    uint64_t phdr_vaddr = 0;
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        elf64_phdr_t* phdr = &phdrs[i];
        
        if (phdr->p_type == PT_PHDR) {
            phdr_vaddr = phdr->p_vaddr;
        }
        
        if (phdr->p_type == PT_LOAD) {
            if (!load_segment(proc, phdr, file_data)) {
                kfree(file_data);
                process_destroy(proc);
                return NULL;
            }
            
            // Update brk if this segment extends the heap area
            uint64_t segment_end = phdr->p_vaddr + phdr->p_memsz;
            if (segment_end > proc->brk) {
                proc->brk = PAGE_ALIGN_UP(segment_end);
                proc->brk_start = proc->brk;
            }
        }
    }
    
    // Setup user stack
    if (!setup_stack(proc, argv, envp, phdr_vaddr, ehdr->e_phentsize, 
                    ehdr->e_phnum, ehdr->e_entry)) {
        kfree(file_data);
        process_destroy(proc);
        return NULL;
    }
    
    // Set entry point
    proc->context.rip = ehdr->e_entry;
    proc->context.rsp = proc->user_stack;
    proc->context.cs = 0x1B;  // User code segment (GDT entry 3, RPL=3)
    proc->context.ss = 0x23;  // User data segment (GDT entry 4, RPL=3)
    proc->context.rflags = 0x202; // Interrupts enabled
    
    kfree(file_data);
    return proc;
}