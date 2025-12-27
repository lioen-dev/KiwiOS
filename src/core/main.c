#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/io.h"
#include "arch/x86/tss.h"
#include "core/boot.h"
#include "core/console.h"
#include "core/keyboard.h"
#include "core/log.h"
#include "core/shell.h"
#include "libc/string.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

static void init_pic(void) {
    // Initialize PIC (Programmable Interrupt Controller)
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    // Mask all interrupts initially
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    // Unmask only IRQ0 (timer)
    outb(0x21, 0xFE);
}

// Enable x86_64 FPU/SSE for both kernel and userspace.
static void x86_enable_sse(void) {
    uint64_t cr0, cr4;
    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));

    // CR0: clear EM (bit 2) to enable FPU, set MP (bit 1) for proper WAIT/FWAIT.
    cr0 &= ~(1ULL << 2);
    cr0 |=  (1ULL << 1);

    // CR4: enable OS support for FXSAVE/FXRSTOR and SIMD exception handling.
    cr4 |= (1ULL << 9);   // OSFXSR
    cr4 |= (1ULL << 10);  // OSXMMEXCPT

    asm volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    asm volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");

    // Initialize FPU state.
    asm volatile ("fninit");
}

void kmain(void) {
    if (!boot_limine_supported()) {
        boot_hcf();
    }

    struct limine_hhdm_response *hhdm = boot_hhdm_response();
    if (!hhdm || hhdm->offset == 0) {
        boot_hcf();
    }
    hhdm_set_offset(hhdm->offset);

    console_init();
    log_ok("console", "Framebuffer console initialized");

    // Disable interrupts during initialization
    asm volatile ("cli");

    init_idt();
    log_ok("interrupts", "IDT installed");

    tss_init();
    gdt_init();
    log_ok("cpu", "GDT/TSS configured");

    x86_enable_sse();
    log_ok("cpu", "SSE enabled");

    struct limine_memmap_response *memmap = boot_memmap_response();
    if (memmap) {
        pmm_init(memmap);
        log_ok("memory", "Physical memory manager ready");
    } else {
        log_error("memory", "No Limine memory map provided");
        boot_hcf();
    }

    vmm_init();
    heap_init();
    log_ok("memory", "Virtual memory and heap initialized");

    init_pic();
    log_info("interrupts", "PIC initialized and timer unmasked");

    // Enable interrupts
    asm volatile ("sti");
    log_info("kernel", "Interrupts enabled");

    shell_loop(console_primary_framebuffer());
    
    // We should never return here; halt if we do.
    while (1) { asm volatile ("hlt"); }
}
