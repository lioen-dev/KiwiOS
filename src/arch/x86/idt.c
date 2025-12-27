#include <stdint.h>
#include <stddef.h>
#include "arch/x86/idt.h"
#include "core/console.h"
#include "libc/string.h"

// ================= IDT (Interrupt Descriptor Table) =================
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT_ENTRIES 256
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtr;

// Exception frame pushed by CPU
struct exception_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

// Exception names
static const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

// A tiny helper the compiler knows never returns
__attribute__((noreturn)) static void panic_halt_forever(void) {
    // Disable interrupts and halt forever
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

// Kernel panic handler
__attribute__((noinline)) void kernel_panic(struct exception_frame *frame) {
    uint32_t old_fg, old_bg;
    console_get_colors(&old_fg, &old_bg);

    // Set panic colors, then reset scrollback so the buffer uses panic colors
    console_set_colors(0x00FFFFFF, 0x00913030);
    console_reset_scrollback();

    // Ensure the scrollback buffer's visible lines use the panic bg
    console_clear_outputs();
    console_render_visible();

    print(NULL, "\n  :3 uh oh, KERNEL PANIC!\n");
    print(NULL, "===========================\n\n");

    // Exception info
    print(NULL, "Exception: ");
    if (frame->int_no < 32) {
        print(NULL, exception_messages[frame->int_no]);
    } else {
        print(NULL, "Unknown Exception");
    }
    print(NULL, "\n");

    print(NULL, "Exception Number: "); print_hex(NULL, frame->int_no); print(NULL, "\n");
    print(NULL, "Error Code: ");       print_hex(NULL, frame->error_code); print(NULL, "\n\n");

    // Registers (add a few useful ones)
    print(NULL, "Register Dump:\n");
    print(NULL, "RIP: "); print_hex(NULL, frame->rip);    print(NULL, "   CS: ");    print_hex(NULL, frame->cs);     print(NULL, "\n");
    print(NULL, "RSP: "); print_hex(NULL, frame->rsp);    print(NULL, "   SS: ");    print_hex(NULL, frame->ss);     print(NULL, "\n");
    print(NULL, "RFLAGS: "); print_hex(NULL, frame->rflags);                        print(NULL, "\n");
    print(NULL, "RBP: "); print_hex(NULL, frame->rbp);    print(NULL, "   CR2: ");   {
        uint64_t cr2 = 0; asm volatile("mov %%cr2, %0" : "=r"(cr2)); print_hex(NULL, cr2);
    }           print(NULL, "\n");
    print(NULL, "RAX: "); print_hex(NULL, frame->rax);    print(NULL, "   RBX: ");   print_hex(NULL, frame->rbx);    print(NULL, "\n");
    print(NULL, "RCX: "); print_hex(NULL, frame->rcx);    print(NULL, "   RDX: ");   print_hex(NULL, frame->rdx);    print(NULL, "\n");
    print(NULL, "RSI: "); print_hex(NULL, frame->rsi);    print(NULL, "   RDI: ");   print_hex(NULL, frame->rdi);    print(NULL, "\n");
    print(NULL, "R8 : "); print_hex(NULL, frame->r8);     print(NULL, "   R9 : ");   print_hex(NULL, frame->r9);     print(NULL, "\n");
    print(NULL, "R10: "); print_hex(NULL, frame->r10);    print(NULL, "   R11: ");   print_hex(NULL, frame->r11);    print(NULL, "\n");
    print(NULL, "R12: "); print_hex(NULL, frame->r12);    print(NULL, "   R13: ");   print_hex(NULL, frame->r13);    print(NULL, "\n");
    print(NULL, "R14: "); print_hex(NULL, frame->r14);    print(NULL, "   R15: ");   print_hex(NULL, frame->r15);    print(NULL, "\n");

    print(NULL, "\nSystem Halted.\n");

    console_set_colors(old_fg, old_bg);
    panic_halt_forever();
}

// Common exception handler
extern void exception_handler_common(void);

// Macro to create exception handlers
#define EXCEPTION_HANDLER(num) \
    extern void exception_##num(void); \
    __attribute__((naked)) void exception_##num(void) { \
        asm volatile ( \
            "push $0\n" \
            "push $" #num "\n" \
            "jmp exception_handler_common\n" \
        ); \
    }

#define EXCEPTION_HANDLER_ERR(num) \
    extern void exception_##num(void); \
    __attribute__((naked)) void exception_##num(void) { \
        asm volatile ( \
            "push $" #num "\n" \
            "jmp exception_handler_common\n" \
        ); \
    }

// Define all exception handlers
EXCEPTION_HANDLER(0)
EXCEPTION_HANDLER(1)
EXCEPTION_HANDLER(2)
EXCEPTION_HANDLER(3)
EXCEPTION_HANDLER(4)
EXCEPTION_HANDLER(5)
EXCEPTION_HANDLER(6)
EXCEPTION_HANDLER(7)
EXCEPTION_HANDLER_ERR(8)
EXCEPTION_HANDLER(9)
EXCEPTION_HANDLER_ERR(10)
EXCEPTION_HANDLER_ERR(11)
EXCEPTION_HANDLER_ERR(12)
EXCEPTION_HANDLER_ERR(13)
EXCEPTION_HANDLER_ERR(14)
EXCEPTION_HANDLER(15)
EXCEPTION_HANDLER(16)
EXCEPTION_HANDLER_ERR(17)
EXCEPTION_HANDLER(18)
EXCEPTION_HANDLER(19)
EXCEPTION_HANDLER(20)
EXCEPTION_HANDLER_ERR(21)

// Common handler that saves all registers
__attribute__((naked)) void exception_handler_common(void) {
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
        "mov %rsp, %rdi\n"  // Pass frame pointer as first argument
        "call kernel_panic\n"
        "add $120, %rsp\n"   // pop 15 regs
        "add $16, %rsp\n"    // pop int_no + error_code
        "iretq\n"
    );
}

__attribute__((naked)) void irq0_handler(void) {
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
        
        "mov %rsp, %rdi\n"  // Pass pointer to interrupt frame
        
        // Send EOI to PIC (AT&T: use DX as the port register)
        "mov $0x20, %al\n"
        "mov $0x20, %dx\n"
        "out %al, (%dx)\n"
        
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

// PCI HDA interrupt handler (wired via legacy PIC / Interrupt Line)
__attribute__((naked)) void irq_hda_handler() {
    __asm__ volatile (
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

        // hda_interrupt_handler(void) doesn't take arguments
        "call hda_interrupt_handler\n"

        // Send EOI: slave then master (safe even if on master only)
        "mov $0x20, %al\n"
        "out %al, $0xA0\n"
        "out %al, $0x20\n"

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

__attribute__((weak)) void hda_interrupt_handler(void) {}

// Set an IDT entry
static void idt_set_gate(uint8_t num, uint64_t handler) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].selector = 0x08; // Kernel code segment
    idt[num].ist = 0;
    idt[num].type_attr = 0x8E; // Present, ring 0, interrupt gate
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].zero = 0;
}

// Initialize IDT
void init_idt(void) {
    // Clear IDT
    memset(idt, 0, sizeof(idt));
    
    // Install exception handlers
    idt_set_gate(0, (uint64_t)exception_0);
    idt_set_gate(1, (uint64_t)exception_1);
    idt_set_gate(2, (uint64_t)exception_2);
    idt_set_gate(3, (uint64_t)exception_3);
    idt_set_gate(4, (uint64_t)exception_4);
    idt_set_gate(5, (uint64_t)exception_5);
    idt_set_gate(6, (uint64_t)exception_6);
    idt_set_gate(7, (uint64_t)exception_7);
    idt_set_gate(8, (uint64_t)exception_8);
    idt_set_gate(9, (uint64_t)exception_9);
    idt_set_gate(10, (uint64_t)exception_10);
    idt_set_gate(11, (uint64_t)exception_11);
    idt_set_gate(12, (uint64_t)exception_12);
    idt_set_gate(13, (uint64_t)exception_13);
    idt_set_gate(14, (uint64_t)exception_14);
    idt_set_gate(15, (uint64_t)exception_15);
    idt_set_gate(16, (uint64_t)exception_16);
    idt_set_gate(17, (uint64_t)exception_17);
    idt_set_gate(18, (uint64_t)exception_18);
    idt_set_gate(19, (uint64_t)exception_19);
    idt_set_gate(20, (uint64_t)exception_20);
    idt_set_gate(21, (uint64_t)exception_21);
    
    // Timer IRQ (IRQ 0 = interrupt 32)
    idt_set_gate(32, (uint64_t)irq0_handler);

    // After setting up all exception handlers, set syscall differently:
    idt[0x80].type_attr = 0xEE; // Change to DPL=3 (ring 3 can call)

    // Load IDT
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    
    asm volatile ("lidt %0" : : "m"(idtr));
}
