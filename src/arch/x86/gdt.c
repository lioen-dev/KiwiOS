#include <stddef.h>
#include <stdint.h>
#include "lib/string.h"
#include "arch/x86/tss.h"

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

#define GDT_ENTRIES 7  // 5 regular + TSS takes 2 entries
static uint64_t gdt[GDT_ENTRIES];
static gdt_ptr_t gdt_ptr;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entry_t* entry = (gdt_entry_t*)&gdt[num];
    entry->base_low = (base & 0xFFFF);
    entry->base_mid = (base >> 16) & 0xFF;
    entry->base_high = (base >> 24) & 0xFF;
    entry->limit_low = (limit & 0xFFFF);
    entry->granularity = (limit >> 16) & 0x0F;
    entry->granularity |= gran & 0xF0;
    entry->access = access;
}

void gdt_init(void) {
    memset(&gdt, 0, sizeof(gdt));
    
    gdt_set_gate(0, 0, 0, 0, 0);                // Null
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // Kernel code (0x08)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Kernel data (0x10)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xAF); // User code (0x18)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User data (0x20)
    
    // Set up TSS descriptor at index 5 (takes 2 entries in 64-bit mode)
    uint64_t tss_base = (uint64_t)&tss;
    uint32_t tss_limit = sizeof(tss_t) - 1;

    // Build the TSS descriptor carefully to avoid shift warnings
    // Lower 64 bits of TSS descriptor
    uint64_t tss_low = 0;
    tss_low |= (tss_limit & 0xFFFF);                    // Limit [15:0]
    tss_low |= ((tss_base & 0xFFFF) << 16);             // Base [15:0]
    tss_low |= (((tss_base >> 16) & 0xFF) << 32);       // Base [23:16]
    tss_low |= (0x89ULL << 40);                         // Type (0x89 = available TSS)
    tss_low |= ((uint64_t)((tss_limit >> 16) & 0xF) << 48);  // Limit [19:16]
    tss_low |= (((tss_base >> 24) & 0xFF) << 56);       // Base [31:24]

    // Upper 64 bits (just the high 32 bits of the base address)
    uint64_t tss_high = (tss_base >> 32);

    gdt[5] = tss_low;
    gdt[6] = tss_high;
    
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)&gdt;
    
    // Load GDT
    asm volatile ("lgdt %0" : : "m"(gdt_ptr) : "memory");
    
    // Reload segments
    asm volatile (
        "pushq $0x08\n"
        "pushq $1f\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        ::: "rax", "memory"
    );
    
    // Load TSS
    uint16_t tss_selector = 0x28;
    asm volatile ("ltr %0" : : "r"(tss_selector) : "memory");
}