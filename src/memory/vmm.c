#include "memory/vmm.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "libc/string.h"
#include <stdint.h>
#include <stdbool.h>

// Get index into page table from virtual address
#define PML4_INDEX(virt) (((virt) >> 39) & 0x1FF)
#define PDPT_INDEX(virt) (((virt) >> 30) & 0x1FF)
#define PD_INDEX(virt)   (((virt) >> 21) & 0x1FF)
#define PT_INDEX(virt)   (((virt) >> 12) & 0x1FF)

// Extract physical address from page table entry
#define PTE_GET_ADDR(entry) ((entry) & 0x000FFFFFFFFFF000ULL)
#define PTE_GET_FLAGS(entry) ((entry) & 0xFFF)

// Current kernel page table (set by Limine)
static page_table_t* kernel_page_table = NULL;

void vmm_init(void) {
    // Limine already set up paging for us
    // We just need to get the current CR3 value (PML4 address)
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));

    // Create kernel page table structure in the heap and zero it out
    kernel_page_table = (page_table_t*)kcalloc(1, sizeof(page_table_t));
    if (!kernel_page_table) return;

    kernel_page_table->pml4_phys = (uint64_t*)(cr3 & ~0xFFFULL);
    kernel_page_table->pml4_virt = phys_to_virt((uint64_t)kernel_page_table->pml4_phys);
    if (!kernel_page_table->pml4_virt) {
        kfree(kernel_page_table);
        kernel_page_table = NULL;
    }
}

page_table_t* vmm_get_kernel_page_table(void) {
    return kernel_page_table;
}

void vmm_switch_page_table(page_table_t* pt) {
    if (!pt) return;
    asm volatile ("mov %0, %%cr3" : : "r"(pt->pml4_phys) : "memory");
}

page_table_t* vmm_create_page_table(void) {
    // Allocate structure
    page_table_t* pt = (page_table_t*)kcalloc(1, sizeof(page_table_t));
    if (!pt) return NULL;

    // Allocate PML4
    uint64_t pml4_phys = (uint64_t)pmm_alloc();
    if (!pml4_phys) {
        kfree(pt);
        return NULL;
    }
    
    pt->pml4_phys = (uint64_t*)pml4_phys;
    pt->pml4_virt = phys_to_virt(pml4_phys);
    if (!pt->pml4_virt) {
        pmm_free((void*)pml4_phys);
        kfree(pt);
        return NULL;
    }
    
    // Clear the PML4
    memset(pt->pml4_virt, 0, PAGE_SIZE);
    
    // Copy kernel mappings (higher half) from current page table
    if (kernel_page_table) {
        // Copy upper half entries (256-511) for kernel space
        for (int i = 256; i < 512; i++) {
            pt->pml4_virt[i] = kernel_page_table->pml4_virt[i];
        }
    }
    
    return pt;
}

// Helper: Get or create a page table at the given entry
static uint64_t* get_or_create_table(uint64_t* table, size_t index, bool user_accessible) {
    uint64_t entry = table[index];

    if (entry & PAGE_PRESENT) {
        // Table already exists - upgrade permissions if needed
        if (user_accessible && !(entry & PAGE_USER)) {
            table[index] |= PAGE_USER;
        }
        return (uint64_t*)phys_to_virt(PTE_GET_ADDR(entry));
    }

    // Need to create a new table
    uint64_t new_table_phys = (uint64_t)pmm_alloc();
    if (!new_table_phys) return NULL;

    uint64_t* new_table_virt = phys_to_virt(new_table_phys);
    memset(new_table_virt, 0, PAGE_SIZE);

    // Set the entry to point to the new table
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE;
    if (user_accessible) {
        flags |= PAGE_USER;
    }
    table[index] = new_table_phys | flags;

    return new_table_virt;
}

bool vmm_map_page(page_table_t* pt, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pt) return false;
    
    // Align addresses
    virt = PAGE_ALIGN_DOWN(virt);
    phys = PAGE_ALIGN_DOWN(phys);
    
    // Get indices
    size_t pml4_idx = PML4_INDEX(virt);
    size_t pdpt_idx = PDPT_INDEX(virt);
    size_t pd_idx = PD_INDEX(virt);
    size_t pt_idx = PT_INDEX(virt);
    
    // Walk/create page tables
    bool user_accessible = (flags & PAGE_USER) != 0;

    uint64_t* pdpt = get_or_create_table(pt->pml4_virt, pml4_idx, user_accessible);
    if (!pdpt) return false;

    uint64_t* pd = get_or_create_table(pdpt, pdpt_idx, user_accessible);
    if (!pd) return false;

    uint64_t* page_table = get_or_create_table(pd, pd_idx, user_accessible);
    if (!page_table) return false;
    
    // Set the final page table entry
    page_table[pt_idx] = phys | flags | PAGE_PRESENT;
    
    // Flush TLB for this address
    asm volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    
    return true;
}

void vmm_unmap_page(page_table_t* pt, uint64_t virt) {
    if (!pt) return;
    
    virt = PAGE_ALIGN_DOWN(virt);
    
    // Get indices
    size_t pml4_idx = PML4_INDEX(virt);
    size_t pdpt_idx = PDPT_INDEX(virt);
    size_t pd_idx = PD_INDEX(virt);
    size_t pt_idx = PT_INDEX(virt);
    
    // Walk page tables (don't create if they don't exist)
    uint64_t pml4_entry = pt->pml4_virt[pml4_idx];
    if (!(pml4_entry & PAGE_PRESENT)) return;
    
    uint64_t* pdpt = phys_to_virt(PTE_GET_ADDR(pml4_entry));
    uint64_t pdpt_entry = pdpt[pdpt_idx];
    if (!(pdpt_entry & PAGE_PRESENT)) return;
    
    uint64_t* pd = phys_to_virt(PTE_GET_ADDR(pdpt_entry));
    uint64_t pd_entry = pd[pd_idx];
    if (!(pd_entry & PAGE_PRESENT)) return;
    
    uint64_t* page_table = phys_to_virt(PTE_GET_ADDR(pd_entry));
    
    // Clear the entry
    page_table[pt_idx] = 0;
    
    // Flush TLB
    asm volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

uint64_t vmm_get_physical(page_table_t* pt, uint64_t virt) {
    if (!pt) return 0;
    
    virt = PAGE_ALIGN_DOWN(virt);
    
    // Get indices
    size_t pml4_idx = PML4_INDEX(virt);
    size_t pdpt_idx = PDPT_INDEX(virt);
    size_t pd_idx = PD_INDEX(virt);
    size_t pt_idx = PT_INDEX(virt);
    
    // Walk page tables
    uint64_t pml4_entry = pt->pml4_virt[pml4_idx];
    if (!(pml4_entry & PAGE_PRESENT)) return 0;
    
    uint64_t* pdpt = phys_to_virt(PTE_GET_ADDR(pml4_entry));
    uint64_t pdpt_entry = pdpt[pdpt_idx];
    if (!(pdpt_entry & PAGE_PRESENT)) return 0;
    
    uint64_t* pd = phys_to_virt(PTE_GET_ADDR(pdpt_entry));
    uint64_t pd_entry = pd[pd_idx];
    if (!(pd_entry & PAGE_PRESENT)) return 0;
    
    uint64_t* page_table = phys_to_virt(PTE_GET_ADDR(pd_entry));
    uint64_t pt_entry = page_table[pt_idx];
    if (!(pt_entry & PAGE_PRESENT)) return 0;
    
    return PTE_GET_ADDR(pt_entry);
}