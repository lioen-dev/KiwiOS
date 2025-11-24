#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch/x86/gdt.h"
#include "limine.h"
#include "font8x8_basic.h"
#include "memory/pmm.h"
#include "arch/x86/tss.h"
#include "memory/vmm.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include "core/process.h"
#include "core/scheduler.h"
#include "drivers/timer.h"
#include "arch/x86/io.h"
#include "exec/elf.h"
#include "core/syscall.h"
#include "drivers/pci.h"
#include "drivers/blockdev.h"
#include "drivers/ata.h"
#include "fs/mbr.h"
#include "fs/ext2.h"
#include "drivers/ahci.h"
#include "drivers/acpi.h"

#include "drivers/hda.h"

#include "lib/string.h"


// ================= Limine boilerplate =================
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

// === Power control helpers (works in QEMU/Bochs/VirtualBox/PCs) ===

static inline void kbd_wait_input_empty(void) {
    // Wait until the 8042 input buffer is empty (bit1 == 0)
    while (inb(0x64) & 0x02) { }
}

// ================= Halt =================
static void hcf(void) {
    for (;;) asm ("hlt");
}

// ================= Framebuffer helpers =================
struct limine_framebuffer *fb0(void) {
    if (framebuffer_request.response == NULL) return NULL;
    if (framebuffer_request.response->framebuffer_count < 1) return NULL;
    return framebuffer_request.response->framebuffers[0];
}

// ================= Multi-output (HDMI/DP/etc.) framebuffer support =================
// We mirror (duplicate) text to all framebuffers Limine exposes.

#define MAX_OUTPUTS 8
#define GLYPH_W 8
#define GLYPH_H 8

static struct limine_framebuffer *g_fbs[MAX_OUTPUTS];
static uint32_t g_fb_count = 0;

// Text layout bounds shared by all outputs (min width/height across displays)
static uint32_t g_text_w_px = 0;  // usable width in pixels (min across outputs)
static uint32_t g_text_h_px = 0;  // usable height in pixels (min across outputs)

// Call this once early in kmain(), after Limine is ready.
static void display_init(void) {
    struct limine_framebuffer_response *resp = framebuffer_request.response;
    if (!resp || resp->framebuffer_count == 0) {
        // Nothing to draw on; halt.
        asm volatile("cli; hlt");
    }

    g_fb_count = (resp->framebuffer_count > MAX_OUTPUTS)
               ? MAX_OUTPUTS : (uint32_t)resp->framebuffer_count;

    // Collect outputs and compute shared usable region (min width/height)
    g_text_w_px = 0xFFFFFFFFu;
    g_text_h_px = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < g_fb_count; i++) {
        g_fbs[i] = resp->framebuffers[i];

        // We assume 32-bpp linear RGB (Limine default GOP/VBE). Your code already assumes 32-bpp.
        // If a display isn't 32-bpp, we just ignore it for safety.
        if (g_fbs[i]->bpp != 32) {
            // Disable this output
            for (uint32_t j = i + 1; j < g_fb_count; j++) g_fbs[j-1] = g_fbs[j];
            g_fb_count--;
            i--;
            continue;
        }

        if (g_fbs[i]->width  < g_text_w_px) g_text_w_px = (uint32_t)g_fbs[i]->width;
        if (g_fbs[i]->height < g_text_h_px) g_text_h_px = (uint32_t)g_fbs[i]->height;
    }

    if (g_fb_count == 0) {
        asm volatile("cli; hlt");
    }

    // Round down to glyph grid so wrapping/scrolling is identical on all displays.
    g_text_w_px = (g_text_w_px / GLYPH_W) * GLYPH_W;
    g_text_h_px = (g_text_h_px / GLYPH_H) * GLYPH_H;
}

// ================= Text renderer (mirrored to all outputs) =================
// Fast *actual* scrolling (VRAM block moves), plus runtime scale (1x,2x,3x,...).

static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint32_t fg_color = 0x00FFFFFF; // white
static uint32_t bg_color = 0x00000000; // black

// Keep this so existing code that references it still compiles, but we don't use it.
static uint32_t g_scroll_origin_px = 0;

// Integer text scale (1=normal, 2=double, ...)
static uint32_t g_scale = 1;
static inline uint32_t CELL_W(void){ return GLYPH_W * g_scale; }
static inline uint32_t CELL_H(void){ return GLYPH_H * g_scale; }

// ---- helpers ----
static inline void fill_row_span(uint8_t *row_base, uint32_t pixels, uint32_t color) {
    uint32_t *p = (uint32_t *)row_base;
    for (uint32_t x = 0; x < pixels; x++) p[x] = color;
}

// Actually scroll the visible text area up by one *cell* (scale-aware) using VRAM memmoves
static void scroll_up_fast_vram(void) {
    const uint32_t step = CELL_H();                // rows to scroll in pixels
    if (step == 0 || step > g_text_h_px) return;

    for (uint32_t i = 0; i < g_fb_count; i++) {
        struct limine_framebuffer *out = g_fbs[i];
        uint8_t *base   = (uint8_t *)(uintptr_t)out->address;
        size_t   pitch  = (size_t)out->pitch;
        size_t   span   = (size_t)g_text_w_px * 4; // copy only the shared text width

        // Move rows [step .. g_text_h_px-1] to [0 .. g_text_h_px-step-1]
        for (uint32_t dst_y = 0; dst_y + step < g_text_h_px; dst_y++) {
            uint8_t *dst = base + (size_t)dst_y       * pitch;
            uint8_t *src = base + (size_t)(dst_y+step)* pitch;
            memmove(dst, src, span);
        }

        // Clear the new bottom band [g_text_h_px-step .. g_text_h_px)
        for (uint32_t y = g_text_h_px - step; y < g_text_h_px; y++) {
            uint8_t *row = base + (size_t)y * pitch;
            fill_row_span(row, g_text_w_px, bg_color);
        }
    }
}

// Draw a scaled glyph directly into each framebuffer (write-only)
static void draw_char_scaled(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font8x8_basic[(uint8_t)c];

    for (uint32_t i = 0; i < g_fb_count; i++) {
        struct limine_framebuffer *out = g_fbs[i];
        if (x + CELL_W() > out->width || y + CELL_H() > out->height) continue;

        uint8_t  *base   = (uint8_t *)(uintptr_t)out->address;
        size_t    pitch  = (size_t)out->pitch;

        // Fill glyph background box
        for (uint32_t ry = 0; ry < CELL_H(); ry++) {
            uint8_t *row = base + (size_t)(y + ry) * pitch + (size_t)x * 4;
            fill_row_span(row, CELL_W(), bg);
        }

        // Plot foreground pixels scaled up
        for (int src_row = 0; src_row < GLYPH_H; src_row++) {
            uint8_t bits = glyph[src_row];
            for (int src_col = 0; src_col < GLYPH_W; src_col++) {
                if (bits & 1) {
                    // Draw a g_scale x g_scale block
                    for (uint32_t dy = 0; dy < g_scale; dy++) {
                        uint8_t *row = base
                                     + (size_t)(y + (uint32_t)src_row * g_scale + dy) * pitch
                                     + (size_t)(x + (uint32_t)src_col * g_scale) * 4;
                        uint32_t *p = (uint32_t *)row;
                        for (uint32_t dx = 0; dx < g_scale; dx++) p[dx] = fg;
                    }
                }
                bits >>= 1;
            }
        }
    }
}

// Public: allow shell to change scale
void console_set_scale(uint32_t new_scale) {
    if (new_scale == 0) new_scale = 1;
    if (new_scale > 16) new_scale = 16;
    if (new_scale == g_scale) return;

    g_scale = new_scale;

    // Reset layout after scale change
    cursor_x = 0; cursor_y = 0;

    // Clear full text area on all outputs
    for (uint32_t i = 0; i < g_fb_count; i++) {
        struct limine_framebuffer *out = g_fbs[i];
        uint8_t *base = (uint8_t *)(uintptr_t)out->address;
        size_t   pitch = (size_t)out->pitch;
        for (uint32_t y = 0; y < g_text_h_px; y++) {
            uint8_t *row = base + (size_t)y * pitch;
            fill_row_span(row, g_text_w_px, bg_color);
        }
    }
}

// --- Required exports (same names as your existing code) ---

void scroll_up(struct limine_framebuffer *fb /*unused*/) {
    (void)fb;
    scroll_up_fast_vram();
}

void draw_char(struct limine_framebuffer *fb /*unused*/,
               uint32_t x, uint32_t y,
               char c, uint32_t fg, uint32_t bg) {
    (void)fb;
    draw_char_scaled(x, y, c, fg, bg);
}

// Draw char at cursor (advances cursor) â€” mirrored to all outputs
void putc_fb(struct limine_framebuffer *fb /*unused*/, char c) {
    (void)fb;

    if (c == '\n') {
        cursor_x = 0;
        cursor_y += CELL_H();
        if (cursor_y + CELL_H() > g_text_h_px) {
            scroll_up(NULL);
            cursor_y = g_text_h_px - CELL_H();
        }
        return;
    }

    if (c == '\b') {
        if (cursor_x >= CELL_W()) {
            cursor_x -= CELL_W();
            draw_char_scaled(cursor_x, cursor_y, ' ', fg_color, bg_color);
        }
        return;
    }

    if (cursor_x + CELL_W() > g_text_w_px) {
        cursor_x = 0;
        cursor_y += CELL_H();
        if (cursor_y + CELL_H() > g_text_h_px) {
            scroll_up(NULL);
            cursor_y = g_text_h_px - CELL_H();
        }
    }

    draw_char_scaled(cursor_x, cursor_y, c, fg_color, bg_color);
    cursor_x += CELL_W();
}

// Draw string at cursor
void print(struct limine_framebuffer *fb /*unused*/, const char *s) {
    (void)fb;
    while (*s) putc_fb(NULL, *s++);
}

// Print a 64-bit hex number (unchanged)
void print_hex(struct limine_framebuffer *fb, uint64_t num) {
    print(fb, "0x");
    static const char hex[] = "0123456789ABCDEF";
    char buf[17]; buf[16] = '\0';
    for (int i = 15; i >= 0; i--) { buf[i] = hex[num & 0xF]; num >>= 4; }
    print(fb, buf);
}

// --- minimal local hex helpers (no "0x", zero-padded) ---
static void print_hex_n_noprefix(struct limine_framebuffer *fb, uint64_t v, int digits) {
    static const char H[] = "0123456789ABCDEF";
    char buf[32];
    for (int i = digits - 1; i >= 0; --i) { buf[i] = H[(uint8_t)(v & 0xF)]; v >>= 4; }
    for (int i = 0; i < digits; ++i) putc_fb(fb, buf[i]);
}

void print_u64(struct limine_framebuffer *fb, uint64_t v) {
    char buf[32];
    int i = 0;
    if (v == 0) { putc_fb(fb, '0'); return; }
    while (v > 0) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i--) putc_fb(fb, buf[i]);
}

void print_u32(struct limine_framebuffer *fb, uint32_t v) {
    print_u64(fb, v);
}

static inline void hx2(struct limine_framebuffer *fb, uint8_t  v){ print_hex_n_noprefix(fb, v, 2); }
static inline void hx4(struct limine_framebuffer *fb, uint16_t v){ print_hex_n_noprefix(fb, v, 4); }
static inline void hx8(struct limine_framebuffer *fb, uint32_t v){ print_hex_n_noprefix(fb, v, 8); }

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

void handle_usermode_exception(struct exception_frame *frame) {
    struct limine_framebuffer *fb = fb0();
    process_t* proc = process_current();
    
    print(fb, "\nUsermode exception in process: ");
    print(fb, proc->name);
    print(fb, "\n");
    
    print(fb, "Exception: ");
    if (frame->int_no < 32) {
        print(fb, exception_messages[frame->int_no]);
    }
    print(fb, "\nRIP: ");
    print_hex(fb, frame->rip);
    print(fb, "\n");
    
    // Terminate the process
    proc->state = PROCESS_TERMINATED;
    
    // Clean up and find next process
    process_cleanup_terminated();
    
    // Try to switch to another process
    process_t* next = process_get_list();
    while (next) {
        if (next->state == PROCESS_READY) {
            print(fb, "Switching to process: ");
            print(fb, next->name);
            print(fb, "\n\n");
            process_switch_to(next);
            return; // Will return to the new process
        }
        next = next->next;
    }
    
    // No other process, go back to idle/shell
    next = process_get_list();
    if (next && next->pid == 0) {
        process_switch_to(next);
    }
}

// A tiny helper the compiler knows never returns
__attribute__((noreturn)) static void panic_halt_forever(void) {
    // Disable interrupts and halt forever
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

// Kernel panic handler
__attribute__((noinline)) void kernel_panic(struct exception_frame *frame) {
    struct limine_framebuffer *fb = fb0();
    if (!fb) panic_halt_forever();

    // User-mode exception? Hand it off and return to the common stub.
    if ((frame->cs & 3) == 3) {
        handle_usermode_exception(frame);
        return; // the scheduler/code should rewrite the frame or pick a new proc
    }

    // --- Kernel-mode fault: collect extra context before drawing ---
    uint64_t cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    uint32_t old_fg = fg_color, old_bg = bg_color;
    fg_color = 0x00FFFFFF;
    bg_color = 0x00FF0000;

    // Clear screen (simple, safe loop)
    uint32_t *fb_ptr = (uint32_t *)(uintptr_t)fb->address;
    size_t pitch = fb->pitch / 4;
    for (uint32_t y = 0; y < fb->height; y++) {
        for (uint32_t x = 0; x < fb->width; x++) {
            fb_ptr[y * pitch + x] = bg_color;
        }
    }
    cursor_x = cursor_y = 0;

    print(fb, "\n  :3 uh oh, KERNEL PANIC!\n");
    print(fb, "===========================\n\n");

    // Exception info
    print(fb, "Exception: ");
    if (frame->int_no < 32) {
        print(fb, exception_messages[frame->int_no]);
    } else {
        print(fb, "Unknown Exception");
    }
    print(fb, "\n");

    print(fb, "Exception Number: "); print_hex(fb, frame->int_no); print(fb, "\n");
    print(fb, "Error Code: ");       print_hex(fb, frame->error_code); print(fb, "\n\n");

    // Registers (add a few useful ones)
    print(fb, "Register Dump:\n");
    print(fb, "RIP: "); print_hex(fb, frame->rip);    print(fb, "   CS: ");    print_hex(fb, frame->cs);     print(fb, "\n");
    print(fb, "RSP: "); print_hex(fb, frame->rsp);    print(fb, "   SS: ");    print_hex(fb, frame->ss);     print(fb, "\n");
    print(fb, "RFLAGS: "); print_hex(fb, frame->rflags);                        print(fb, "\n");
    print(fb, "RBP: "); print_hex(fb, frame->rbp);    print(fb, "   CR2: ");   print_hex(fb, cr2);           print(fb, "\n");
    print(fb, "RAX: "); print_hex(fb, frame->rax);    print(fb, "   RBX: ");   print_hex(fb, frame->rbx);    print(fb, "\n");
    print(fb, "RCX: "); print_hex(fb, frame->rcx);    print(fb, "   RDX: ");   print_hex(fb, frame->rdx);    print(fb, "\n");
    print(fb, "RSI: "); print_hex(fb, frame->rsi);    print(fb, "   RDI: ");   print_hex(fb, frame->rdi);    print(fb, "\n");
    print(fb, "R8 : "); print_hex(fb, frame->r8);     print(fb, "   R9 : ");   print_hex(fb, frame->r9);     print(fb, "\n");
    print(fb, "R10: "); print_hex(fb, frame->r10);    print(fb, "   R11: ");   print_hex(fb, frame->r11);    print(fb, "\n");
    print(fb, "R12: "); print_hex(fb, frame->r12);    print(fb, "   R13: ");   print_hex(fb, frame->r13);    print(fb, "\n");
    print(fb, "R14: "); print_hex(fb, frame->r14);    print(fb, "   R15: ");   print_hex(fb, frame->r15);    print(fb, "\n");

    print(fb, "\nSystem Halted.\n");

    // Restore colors (not strictly necessary now)
    fg_color = old_fg; bg_color = old_bg;

    // Never return on a kernel panic
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

extern void timer_handler(uint64_t* interrupt_rsp);
extern void hda_irq_handler(uint64_t* interrupt_rsp);

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
        "call timer_handler\n"
        
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

__attribute__((naked)) void irq_hda_handler(void) {
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
        "call hda_irq_handler\n"

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

// Set an IDT entry
void idt_set_gate(uint8_t num, uint64_t handler) {
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

    extern void syscall_handler(void);
    idt_set_gate(0x80, (uint64_t)syscall_handler);

    // After setting up all exception handlers, set syscall differently:
    idt[0x80].type_attr = 0xEE; // Change to DPL=3 (ring 3 can call)

    // Load IDT
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    
    asm volatile ("lidt %0" : : "m"(idtr));
}

// ================= Keyboard driver (PS/2) =================
#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64

// US QWERTY scancode set 1 -> ASCII mapping
static const char scancode_to_ascii[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, // Left ctrl
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, // Left shift
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 
    0, // Right shift
    '*',
    0, // Left alt
    ' '
};

static const char scancode_to_ascii_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, // Left ctrl
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, // Left shift
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 
    0, // Right shift
    '*',
    0, // Left alt
    ' '
};

// Add near your other keyboard state:
static bool shift_pressed = false;
static bool ctrl_pressed  = false;

// Helpers for scancode press/release
static inline bool is_shift_press(uint8_t s)   { return s == 0x2A || s == 0x36; }
static inline bool is_shift_release(uint8_t s) { return s == 0xAA || s == 0xB6; }
static inline bool is_ctrl_press(uint8_t s)    { return s == 0x1D; }   // Left Ctrl
static inline bool is_ctrl_release(uint8_t s)  { return s == 0x9D; }   // Left Ctrl release

static inline char maybe_ctrlify(char c) {
    if (!ctrl_pressed) return c;
    // Only letters become control chars (A..Z -> 1..26)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return (char)(c & 0x1F);
    }
    return c;
}

char keyboard_getchar(void) {
    for (;;) {
        // Data available?
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & 0x01)) continue;

        uint8_t scancode = inb(PS2_DATA_PORT);

        // Track modifiers
        if (is_shift_press(scancode))   { shift_pressed = true;  continue; }
        if (is_shift_release(scancode)) { shift_pressed = false; continue; }
        if (is_ctrl_press(scancode))    { ctrl_pressed  = true;  continue; }
        if (is_ctrl_release(scancode))  { ctrl_pressed  = false; continue; }

        // Ignore generic key releases
        if (scancode & 0x80) continue;

        // Map to ASCII and optionally ctrlify
        if (scancode < sizeof(scancode_to_ascii)) {
            char c = shift_pressed ? scancode_to_ascii_shift[scancode]
                                   : scancode_to_ascii[scancode];
            if (c != 0) return maybe_ctrlify(c);
        }
    }
}

int keyboard_getchar_nonblocking(void) {
    // Any data?
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & 0x01)) return -1;

    uint8_t scancode = inb(PS2_DATA_PORT);

    // Track modifiers
    if (is_shift_press(scancode))   { shift_pressed = true;  return -1; }
    if (is_shift_release(scancode)) { shift_pressed = false; return -1; }
    if (is_ctrl_press(scancode))    { ctrl_pressed  = true;  return -1; }
    if (is_ctrl_release(scancode))  { ctrl_pressed  = false; return -1; }

    // Ignore generic key releases
    if (scancode & 0x80) return -1;

    // Map to ASCII and optionally ctrlify
    if (scancode < sizeof(scancode_to_ascii)) {
        char c = shift_pressed ? scancode_to_ascii_shift[scancode]
                               : scancode_to_ascii[scancode];
        if (c != 0) return (int)maybe_ctrlify(c);
    }
    return -1;
}

void wait_for_key(void) {
    struct limine_framebuffer *fb = fb0();
    print(fb, "[Press any key to continue]");
    keyboard_getchar();
    print(fb, "\n");
}

// ================= Command functions =================
void cmd_help(struct limine_framebuffer *fb) {
    print(fb, "Available commands:\n\n");
    print(fb, "  help       - Show this help message\n");
    print(fb, "  clear      - Clear the screen\n");
    print(fb, "  about      - Show information about KiwiOS\n");
    print(fb, "  echo [msg] - Print message to the screen\n");
    print(fb, "  shutdown   - Shutdown the system\n");
    print(fb, "  reboot     - Reboot the system\n");
    print(fb, "  pcilist    - List PCI devices\n");

    print(fb, "\n");

    print(fb, "  [FILESYSTEM COMMANDS]\n");
    print(fb, "  ls [path]       - List directory contents (default: current directory)\n");
    print(fb, "  pwd             - Print working directory\n");
    print(fb, "  cd [path]      - Change directory (default: /)\n");
    print(fb, "  cat <file>     - Display file contents\n");
    print(fb, "  run <file>     - Execute a program\n");
    print(fb, "  touch <file>   - Create an empty file\n");
    print(fb, "  append <file> <text> - Append text to a file\n");
    print(fb, "  truncate <file> <size> - Truncate file to size bytes\n");

    print(fb, "\n");

    print(fb, "  [DEBUGGING COMMANDS]\n");
    print(fb, "  meminfo    - Show memory information\n");
    print(fb, "  memtest    - Run memory test\n");
    print(fb, "  vmtest     - Run virtual memory test\n");
    print(fb, "  heaptest   - Run heap allocation test\n");
    print(fb, "  pslist     - List running processes\n");
    print(fb, "  psdebug    - Show debug info for current process\n");
    print(fb, "  switch     - Switch to next process\n");
    print(fb, "  fbinfo     - Show framebuffer information\n");
    print(fb, "  crash [n]  - Trigger exception number n (default 0)\n");
}

void cmd_clear(struct limine_framebuffer *fb /*unused*/) {
    (void)fb;
    g_scroll_origin_px = 0;
    for (uint32_t i = 0; i < g_fb_count; i++) {
        struct limine_framebuffer *out = g_fbs[i];
        uint32_t *fb_ptr = (uint32_t *)(uintptr_t)out->address;
        size_t pitch32 = (size_t)out->pitch / 4;

        for (uint32_t y = 0; y < out->height; y++) {
            uint32_t *row = fb_ptr + (size_t)y * pitch32;
            for (uint32_t x = 0; x < out->width; x++) row[x] = bg_color;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

void cmd_echo(struct limine_framebuffer *fb, const char *args) {
    if (args && *args) {
        print(fb, args);
        print(fb, "\n");
    } else {
        print(fb, "\n");
    }
}

void cmd_about(struct limine_framebuffer *fb) {
    print(fb, "KiwiOS v0.1\n");
    print(fb, "A simple operating system\n");
}

void cmd_crash(struct limine_framebuffer *fb, const char *args) {
    int exception_num = 0; // default to divide by zero
    
    // Parse exception number from args
    if (args && *args) {
        exception_num = 0; // Reset to 0 when parsing
        // Simple string to int conversion
        while (*args >= '0' && *args <= '9') {
            exception_num = exception_num * 10 + (*args - '0');
            args++;
        }
    }
    
    print(fb, "Triggering exception ");
    print_hex(fb, exception_num);
    print(fb, "...\n");
    
    // Trigger the appropriate exception
    switch (exception_num) {
        case 0: { // Division by zero
            volatile int x = 1;
            volatile int y = 0;
            volatile int z = x / y;
            (void)z;
            break;
        }
        case 1: // Debug - use int instruction
            asm volatile ("int $1");
            break;
        case 2: // Non Maskable Interrupt
            asm volatile ("int $2");
            break;
        case 3: // Breakpoint
            asm volatile ("int3");
            break;
        case 4: // Overflow
            asm volatile ("int $4");
            break;
        case 5: { // Bound range exceeded
            // Note: BOUND instruction removed in x86-64, use int instead
            asm volatile ("int $5");
            break;
        }
        case 6: // Invalid opcode
            asm volatile ("ud2");
            break;
        case 7: { // Device not available (FPU)
            // Try to use FPU without enabling it first
            asm volatile (
                "clts\n"  // Clear task switched flag
                "fninit\n" // Init FPU
                "mov $0, %%rax\n"
                "mov %%rax, %%cr0\n" // Clear CR0.TS
                "fld1\n"  // This might not fault, so use int
                ::: "rax"
            );
            asm volatile ("int $7");
            break;
        }
        case 8: // Double fault - hard to trigger, use int
            asm volatile ("int $8");
            break;
        case 10: { // Invalid TSS
            asm volatile ("int $10");
            break;
        }
        case 11: { // Segment not present
            asm volatile ("int $11");
            break;
        }
        case 12: { // Stack segment fault
            asm volatile ("int $12");
            break;
        }
        case 13: { // General protection fault
            // Try to load invalid segment selector
            asm volatile ("mov $0xFFFF, %%ax; mov %%ax, %%ds" ::: "rax");
            break;
        }
        case 14: { // Page fault
            // Try to access unmapped memory
            volatile uint64_t *ptr = (uint64_t *)0xFFFFFFFF80000000ULL;
            volatile uint64_t val = *ptr;
            (void)val;
            break;
        }
        case 16: { // x87 FPU error
            asm volatile ("int $16");
            break;
        }
        case 17: { // Alignment check
            asm volatile ("int $17");
            break;
        }
        case 18: { // Machine check
            asm volatile ("int $18");
            break;
        }
        case 19: { // SIMD floating point exception
            asm volatile ("int $19");
            break;
        }
        case 20: { // Virtualization exception
            asm volatile ("int $20");
            break;
        }
        case 21: { // Control protection exception
            asm volatile ("int $21");
            break;
        }
        default:
            print(fb, "Exception number not supported or reserved.\n");
            print(fb, "Supported: 0-8, 10-14, 16-21\n");
            return;
    }
}

void cmd_meminfo(struct limine_framebuffer *fb) {
    size_t total, used, free;
    pmm_get_stats(&total, &used, &free);
    
    print(fb, "Memory Information:\n");
    print(fb, "  Total pages: ");
    print_hex(fb, total);
    print(fb, " (");
    print_hex(fb, total * 4); // KB
    print(fb, " KB)\n");
    
    print(fb, "  Used pages:  ");
    print_hex(fb, used);
    print(fb, " (");
    print_hex(fb, used * 4);
    print(fb, " KB)\n");
    
    print(fb, "  Free pages:  ");
    print_hex(fb, free);
    print(fb, " (");
    print_hex(fb, free * 4);
    print(fb, " KB)\n");
}

void cmd_memtest(struct limine_framebuffer *fb) {
    print(fb, "Testing memory allocation...\n");
    
    // Allocate a single page
    void* page1 = pmm_alloc();
    print(fb, "Allocated page at: ");
    print_hex(fb, (uint64_t)page1);
    print(fb, "\n");
    
    // Allocate another page
    void* page2 = pmm_alloc();
    print(fb, "Allocated page at: ");
    print_hex(fb, (uint64_t)page2);
    print(fb, "\n");
    
    // Allocate 10 contiguous pages
    void* pages = pmm_alloc_pages(10);
    if (pages) {
        print(fb, "Allocated 10 pages at: ");
        print_hex(fb, (uint64_t)pages);
        print(fb, "\n");
    } else {
        print(fb, "Failed to allocate 10 pages!\n");
    }
    
    // Free them
    print(fb, "Freeing allocations...\n");
    pmm_free(page1);
    pmm_free(page2);
    if (pages) pmm_free_pages(pages, 10);
    
    print(fb, "Memory test complete!\n");
}

void cmd_vmtest(struct limine_framebuffer *fb) {
    print(fb, "Testing Virtual Memory Manager...\n");
    
    // Create a new page table
    page_table_t* test_pt = vmm_create_page_table();
    if (!test_pt) {
        print(fb, "Failed to create page table!\n");
        return;
    }
    print(fb, "Created page table at: ");
    print_hex(fb, (uint64_t)test_pt);
    print(fb, "\n");
    
    // Allocate a physical page
    uint64_t phys_page = (uint64_t)pmm_alloc();
    if (!phys_page) {
        print(fb, "Failed to allocate physical page!\n");
        return;
    }
    print(fb, "Allocated physical page: ");
    print_hex(fb, phys_page);
    print(fb, "\n");
    
    // Map it to a virtual address (let's use 0x400000, typical userspace)
    uint64_t virt_addr = 0x400000;
    bool mapped = vmm_map_page(test_pt, virt_addr, phys_page, PAGE_WRITE | PAGE_USER);
    if (!mapped) {
        print(fb, "Failed to map page!\n");
        pmm_free((void*)phys_page);
        return;
    }
    print(fb, "Mapped virtual ");
    print_hex(fb, virt_addr);
    print(fb, " -> physical ");
    print_hex(fb, phys_page);
    print(fb, "\n");
    
    // Verify the mapping
    uint64_t phys_result = vmm_get_physical(test_pt, virt_addr);
    if (phys_result == phys_page) {
        print(fb, "Mapping verified successfully!\n");
    } else {
        print(fb, "Mapping verification FAILED!\n");
        print(fb, "Expected: ");
        print_hex(fb, phys_page);
        print(fb, "\nGot: ");
        print_hex(fb, phys_result);
        print(fb, "\n");
    }
    
    // Test unmapping
    vmm_unmap_page(test_pt, virt_addr);
    phys_result = vmm_get_physical(test_pt, virt_addr);
    if (phys_result == 0) {
        print(fb, "Unmapping successful!\n");
    } else {
        print(fb, "Unmapping FAILED!\n");
    }
    
    // Clean up
    pmm_free((void*)phys_page);
    
    print(fb, "VMM test complete!\n");
}

void cmd_heaptest(struct limine_framebuffer *fb) {
    print(fb, "Testing heap allocator...\n");
    
    // Test 1: Simple allocation
    char* str1 = (char*)kmalloc(32);
    if (str1) {
        print(fb, "Allocated 32 bytes at: ");
        print_hex(fb, (uint64_t)str1);
        print(fb, "\n");
    }
    
    // Test 2: Multiple allocations
    int* numbers = (int*)kmalloc(10 * sizeof(int));
    if (numbers) {
        print(fb, "Allocated array at: ");
        print_hex(fb, (uint64_t)numbers);
        print(fb, "\n");
    }
    
    // Test 3: Calloc (zeroed memory)
    uint64_t* zeroed = (uint64_t*)kcalloc(5, sizeof(uint64_t));
    if (zeroed) {
        print(fb, "Allocated zeroed memory at: ");
        print_hex(fb, (uint64_t)zeroed);
        print(fb, "\n");
    }
    
    // Show stats
    size_t allocated, free_mem, allocs;
    heap_get_stats(&allocated, &free_mem, &allocs);
    print(fb, "Heap stats:\n");
    print(fb, "  Allocated: ");
    print_hex(fb, allocated);
    print(fb, " bytes\n");
    print(fb, "  Free: ");
    print_hex(fb, free_mem);
    print(fb, " bytes\n");
    print(fb, "  Active allocations: ");
    print_hex(fb, allocs);
    print(fb, "\n");
    
    // Free everything
    kfree(str1);
    kfree(numbers);
    kfree(zeroed);
    
    print(fb, "Freed all allocations\n");
    
    heap_get_stats(&allocated, &free_mem, &allocs);
    print(fb, "After free - Active allocations: ");
    print_hex(fb, allocs);
    print(fb, "\n");
}

void test_process_1(void) {
    struct limine_framebuffer *fb = fb0();
    
    for (int i = 0; i < 5; i++) {
        print(fb, "Process 1 tick ");
        print_hex(fb, timer_get_ticks());
        print(fb, "\n");
        
        // Busy wait a bit so we can see it
        for (volatile int j = 0; j < 10000000; j++);
    }
    
    print(fb, "Process 1 done\n");
}

void test_process_2(void) {
    struct limine_framebuffer *fb = fb0();
    
    for (int i = 0; i < 5; i++) {
        print(fb, "Process 2 tick ");
        print_hex(fb, timer_get_ticks());
        print(fb, "\n");
        
        // Busy wait a bit
        for (volatile int j = 0; j < 10000000; j++);
    }
    
    print(fb, "Process 2 done\n");
}

void cmd_pslist(struct limine_framebuffer *fb) {
    print(fb, "Process List:\n");
    print(fb, "PID  STATE      NAME\n");
    print(fb, "---  ---------  ----\n");
    
    process_t* proc = process_get_list();
    
    while (proc) {
        print_hex(fb, proc->pid);
        print(fb, "  ");
        
        switch (proc->state) {
            case PROCESS_READY:
                print(fb, "READY     ");
                break;
            case PROCESS_RUNNING:
                print(fb, "RUNNING   ");
                break;
            case PROCESS_TERMINATED:
                print(fb, "TERMINATED");
                break;
        }
        print(fb, "  ");
        
        print(fb, proc->name);
        print(fb, "\n");
        
        proc = proc->next;
    }
}

void cmd_switch(struct limine_framebuffer *fb) {
    process_t* current = process_current();
    print(fb, "Current: ");
    print(fb, current->name);
    print(fb, " (PID ");
    print_hex(fb, current->pid);
    print(fb, ")\n");
    
    // Find next ready process
    process_t* next = current->next;
    if (!next) next = process_get_list();
    
    while (next && next != current) {
        if (next->state == PROCESS_READY) {
            print(fb, "Switching to: ");
            print(fb, next->name);
            print(fb, "\n");
            
            process_switch_to(next);
            
            // When we return here, we've been switched back
            print(fb, "Back to: ");
            print(fb, process_current()->name);
            print(fb, "\n");
            return;
        }
        next = next->next;
        if (!next) next = process_get_list();
    }
    
    print(fb, "No ready processes\n");
}

void cmd_psdebug(struct limine_framebuffer *fb) {
    process_t* proc = process_get_list();
    
    while (proc) {
        print(fb, "Process: ");
        print(fb, proc->name);
        print(fb, "\n");
        print(fb, "  PID: ");
        print_hex(fb, proc->pid);
        print(fb, "\n");
        print(fb, "  RSP: ");
        print_hex(fb, proc->context.rsp);
        print(fb, "\n");
        print(fb, "  Stack Top: ");
        print_hex(fb, proc->stack_top);
        print(fb, "\n\n");
        
        proc = proc->next;
    }
}

// --- fbinfo: list all Limine framebuffers and modes ---
void cmd_fbinfo(struct limine_framebuffer *fb_unused) {
    (void)fb_unused;

    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count == 0) {
        print(NULL, "No framebuffers from Limine.\n");
        return;
    }

    uint64_t count = framebuffer_request.response->framebuffer_count;
    print(NULL, "Framebuffers: ");
    print_u64(NULL, count);
    print(NULL, "\n");

    for (uint64_t i = 0; i < count; i++) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[i];
        if (!fb) continue;

        print(NULL, "FB#"); print_u64(NULL, i); print(NULL, ": ");
        // WxH@bpp (pitch in bytes)
        print_u64(NULL, fb->width);  print(NULL, "x");
        print_u64(NULL, fb->height); print(NULL, "@");
        print_u64(NULL, fb->bpp);    print(NULL, "  pitch=");
        print_u64(NULL, fb->pitch);
        print(NULL, " bytes\n");

        // Memory model and RGB masks
        print(NULL, "  mem_model=");
        print_u64(NULL, fb->memory_model);
        print(NULL, "  R(");
        print_u64(NULL, fb->red_mask_size);   print(NULL, ":");
        print_u64(NULL, fb->red_mask_shift);  print(NULL, ")  G(");
        print_u64(NULL, fb->green_mask_size); print(NULL, ":");
        print_u64(NULL, fb->green_mask_shift);print(NULL, ")  B(");
        print_u64(NULL, fb->blue_mask_size);  print(NULL, ":");
        print_u64(NULL, fb->blue_mask_shift); print(NULL, ")\n");

        // EDID
        print(NULL, "  edid=");
        if (fb->edid && fb->edid_size) {
            print_u64(NULL, fb->edid_size); print(NULL, " bytes\n");
        } else {
            print(NULL, "none\n");
        }

        // Modes (if present)
        if (fb->mode_count && fb->modes) {
            uint64_t mcount = fb->mode_count;
            print(NULL, "  modes=");
            print_u64(NULL, mcount);
            print(NULL, " (showing up to 10)\n");

            uint64_t show = mcount > 10 ? 10 : mcount;
            for (uint64_t j = 0; j < show; j++) {
                struct limine_video_mode *m = fb->modes[j];
                if (!m) continue;

                print(NULL, "    [");
                print_u64(NULL, j);
                print(NULL, "] ");
                print_u64(NULL, m->width);  print(NULL, "x");
                print_u64(NULL, m->height); print(NULL, "@");
                print_u64(NULL, m->bpp);
                print(NULL, "  pitch=");
                print_u64(NULL, m->pitch);
                print(NULL, "  mem_model=");
                print_u64(NULL, m->memory_model);
                print(NULL, "\n");
            }
        } else {
            print(NULL, "  modes=none\n");
        }

        print(NULL, "\n");
    }
}

void cmd_shutdown(struct limine_framebuffer *fb) {
    (void)fb;
    print(NULL, "Shutting down...\n");
    acpi_poweroff(); // no-return
}

void cmd_reboot(struct limine_framebuffer *fb) {
    (void)fb;
    print(NULL, "Rebooting...\n");
    acpi_reboot(); // no-return
}

// --- vendor name lookup (common IDs only; extend as you like) ---
static const struct { uint16_t vid; const char* name; } VENDORS[] = {
    {0x8086,"Intel"}, {0x10DE,"NVIDIA"}, {0x1002,"AMD/ATI"}, {0x1022,"AMD"},
    {0x1AF4,"Red Hat VirtIO"}, {0x80EE,"Oracle VirtualBox"}, {0x15AD,"VMware"},
    {0x1234,"QEMU"}, {0x1B36,"QEMU (PCI-PCIe Bridge)"},
    {0x10EC,"Realtek"}, {0x14E4,"Broadcom"}, {0x1B21,"ASMedia"},
    {0x1912,"Renesas"}, {0x1B4B,"Marvell"}, {0,0}
};
static const char* vendor_name(uint16_t vid){
    for (int i=0; VENDORS[i].name; ++i) if (VENDORS[i].vid==vid) return VENDORS[i].name;
    return "UnknownVendor";
}

// --- class/subclass/progif decoding (condensed, common cases) ---
static const char* class_name(uint8_t cc){
    switch(cc){
        case 0x00: return "Unclassified";
        case 0x01: return "Mass Storage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06: return "Bridge";
        case 0x07: return "Comm";
        case 0x08: return "Base System";
        case 0x09: return "Input";
        case 0x0A: return "Docking";
        case 0x0B: return "Processor";
        case 0x0C: return "Serial Bus";
        case 0x0D: return "Wireless";
        case 0x0E: return "I/O";
        case 0x0F: return "Satellite";
        case 0x10: return "Encryption";
        case 0x11: return "Signal Proc";
        case 0x12: return "Proc Accel";
        case 0x13: return "Non-Essent";
        case 0x40: return "Co-processor";
        case 0xFF: return "Unassigned";
        default:   return "Class?";
    }
}
static const char* subclass_name(uint8_t cc, uint8_t sc){
    switch (cc){
        case 0x01: // Mass Storage
            switch(sc){
                case 0x00: return "SCSI";
                case 0x01: return "IDE";
                case 0x02: return "Floppy";
                case 0x03: return "IPI";
                case 0x04: return "RAID";
                case 0x05: return "ATA";
                case 0x06: return "SATA";
                case 0x07: return "SAS";
                case 0x08: return "NVMHCI";
                case 0x09: return "NVM Express";
                default:   return "Storage?";
            }
        case 0x02: return (sc==0x00)?"Ethernet":(sc==0x80)?"OtherNet":"Net?";
        case 0x03: return (sc==0x00)?"VGA":(sc==0x02)?"3D":"Display?";
        case 0x04:
            switch(sc){
                case 0x00: return "Multimedia Dev";
                case 0x01: return "Audio (Legacy)";
                case 0x02: return "Telephony";
                case 0x03: return "High Def Audio"; // Intel HDA
                case 0x04: return "Video Ctrl";
                default:   return "Multimedia?";
            }
        case 0x06:
            switch(sc){
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-PCI Bridge";
                case 0x07: return "CardBus";
                case 0x09: return "PCI-PCI Bridge";
                default:   return "Bridge?";
            }
        case 0x08:
            switch(sc){
                case 0x00: return "PIC";
                case 0x01: return "DMA";
                case 0x02: return "Timer";
                case 0x03: return "RTC";
                case 0x04: return "PCI Hotplug";
                case 0x05: return "SD Host";
                default:   return "BaseSys?";
            }
        case 0x09:
            switch(sc){
                case 0x00: return "Keyboard";
                case 0x01: return "Digitizer";
                case 0x02: return "Mouse";
                case 0x03: return "Scanner";
                case 0x04: return "Gameport";
                default:   return "Input?";
            }
        case 0x0C: // Serial Bus
            switch(sc){
                case 0x00: return "FireWire";
                case 0x01: return "ACCESS.bus";
                case 0x02: return "SSA";
                case 0x03: return "USB";
                case 0x05: return "SMBus";
                default:   return "SerialBus?";
            }
        default: return "Subclass?";
    }
}
static const char* progif_name(uint8_t cc, uint8_t sc, uint8_t pi){
    if (cc==0x0C && sc==0x03){ // USB
        switch(pi){
            case 0x00: return "UHCI";
            case 0x10: return "OHCI";
            case 0x20: return "EHCI";
            case 0x30: return "XHCI";
            case 0x80: return "UnspecUSB";
            case 0xFE: return "USB Device";
        }
    }
    if (cc==0x01 && sc==0x06){ // SATA
        return (pi==0x01)?"AHCI":"SATA";
    }
    if (cc==0x04 && sc==0x03){ // HDA
        return "HDA";
    }
    return "";
}

// --- pretty printer command ---
void cmd_pcilist(struct limine_framebuffer *fb) {
    pci_device_t devs[256];
    int n = pci_enum_devices(devs, 256);

    print(fb, "Bus:Dev.F  VID:DID   CC.SC.IF  Vendor              -> Device\n");
    print(fb, "---------------------------------------------------------------\n");

    int shown = 0;
    for (int i = 0; i < n; ++i) {
        pci_device_t* d = &devs[i];
        // BDF
        hx2(fb, d->bus); print(fb, ":"); hx2(fb, d->slot); print(fb, "."); hx2(fb, d->func); print(fb, "  ");
        // VID:DID
        hx4(fb, d->vendor_id); print(fb, ":"); hx4(fb, d->device_id); print(fb, "  ");
        // CC.SC.IF
        hx2(fb, d->class_code); print(fb, "."); hx2(fb, d->subclass); print(fb, "."); hx2(fb, d->prog_if); print(fb, "  ");

        const char* vname = vendor_name(d->vendor_id);
        const char* cname = class_name(d->class_code);
        const char* sname = subclass_name(d->class_code, d->subclass);
        const char* pname = progif_name(d->class_code, d->subclass, d->prog_if);

        print(fb, vname); print(fb, "              "); // crude spacing
        print(fb, "-> ");

        // Compose a readable device type
        if (d->class_code==0x04 && d->subclass==0x03) {
            // Make HDA pop
            print(fb, "High Definition Audio");
            if (d->vendor_id==0x8086) print(fb, " (Intel HDA)");
        } else {
            print(fb, cname); print(fb, " / "); print(fb, sname);
            if (pname[0]) { print(fb, " ("); print(fb, pname); print(fb, ")"); }
        }

        print(fb, "\n");
        if (++shown >= 256) break; // safety
    }

    print(fb, "---------------------------------------------------------------\n");
    print(fb, "Total devices listed: "); print_hex(fb, (uint64_t)shown); print(fb, "\n");
}

// lightweight print helpers
static void kputs(const char* s) { print(fb0(), s); }

// FS globals
static ext2_fs_t* g_fs = NULL;

// bootstrap: ATA â†’ first MBR partition â†’ ext2 mount and chdir("/")
static void fs_init(void) {
    block_device_t* root = blockdev_get_root();
    if (!root) { kputs("[disk] no block devices found (AHCI/ATA).\n"); return; }

    block_device_t* part = mbr_open_first_partition(root);
    block_device_t* vol  = part ? part : root;

    g_fs = ext2_mount(vol);
    if (!g_fs) {
        kputs("[ext2] mount failed. Is the device/partition ext2?\n");
        return;
    }
    (void)ext2_chdir(g_fs, "/");
    kputs("[ext2] mounted and set cwd to /\n");
}


// ls [path]
static void _ls_cb(const ext2_dirent_t* e, void* user) {
    (void)user;
    print(fb0(), e->name);
    // ext2_dirent_t uses file_type: 1=regular, 2=directory
    if (e->file_type == 2) print(fb0(), "/");
    print(fb0(), "\n");
}

// take an optional path (args may be NULL/"")
void cmd_ls(struct limine_framebuffer* fb, const char* path) {
    (void)fb;
    if (!g_fs) { kputs("[ext2] not mounted.\n"); return; }
    const char* p = (path && *path) ? path : ".";
    ext2_listdir(g_fs, p, _ls_cb, NULL);
}

void cmd_pwd(struct limine_framebuffer* fb) {
    (void)fb;
    if (!g_fs) { kputs("[ext2] not mounted.\n"); return; }
    kputs(ext2_get_cwd()); kputs("\n");
}

void cmd_cd(struct limine_framebuffer* fb, const char* args) {
    (void)fb;
    if (!g_fs) { kputs("[ext2] not mounted.\n"); return; }
    const char* path = (args && *args) ? args : "/";
    if (!ext2_chdir(g_fs, path)) kputs("cd: no such dir\n");
}

void cmd_cat(struct limine_framebuffer* fb, const char* args) {
    (void)fb;
    if (!g_fs) { kputs("[ext2] not mounted.\n"); return; }
    if (!args || !*args) { kputs("usage: cat <file>\n"); return; }
    size_t sz = 0;
    void* data = ext2_read_entire_file(g_fs, args, &sz);
    if (!data) { kputs("cat: cannot read file\n"); return; }
    char* c = (char*)data;
    for (size_t i = 0; i < sz; i++) {
        char ch = c[i];
        if (ch == '\0') ch = '\n';
        char s[2] = { ch, 0 };
        print(fb0(), s);
    }
    extern void kfree(void* p);
    kfree(data);
}

void cmd_touch(struct limine_framebuffer* fb, const char* args) {
    (void)fb;
    if (!g_fs) { kputs("[ext2] not mounted.\n"); return; }
    if (!args || !*args) { kputs("usage: touch <file>\n"); return; }
    if (!ext2_replace(g_fs, args, "", 0)) kputs("touch: failed\n");
}

// append <file> <text...>  (dispatcher already splits path/text)
void cmd_append(struct limine_framebuffer* fb, const char* path, const char* text) {
    (void)fb;
    if (!g_fs) { kputs("[ext2] not mounted.\n"); return; }
    if (!path || !*path) { kputs("usage: append <file> <text>\n"); return; }
    if (!text) text = "";
    if (!ext2_append(g_fs, path, text, (uint32_t)strlen(text))) kputs("append: failed\n");
}

// truncate <file> <size>  (dispatcher parses size_t for you)
void cmd_truncate(struct limine_framebuffer* fb, const char* path, size_t new_size) {
    (void)fb;
    if (!g_fs) { kputs("[ext2] not mounted.\n"); return; }
    if (!path || !*path) { kputs("usage: truncate <file> <size>\n"); return; }
    if (!ext2_truncate(g_fs, path, (uint32_t)new_size)) kputs("truncate: failed\n");
}

void cmd_run(struct limine_framebuffer* fb, const char* args) {
    (void)fb;
    if (!g_fs) { kputs("[ext2] not mounted.\n"); return; }
    if (!args || !*args) { kputs("usage: run <file.elf> [args...]\n"); return; }

    const char* p = args;
    char prog[256]; size_t n=0;
    while (*p && *p!=' ' && n < sizeof(prog)-1) { prog[n++]=*p++; }
    prog[n]='\0';

    int argc = 0; const char* argv_arr[8];
    argv_arr[argc++] = prog;
    while (*p==' ') p++;
    while (*p && argc < 8) {
        argv_arr[argc++] = p;
        while (*p && *p!=' ') p++;
        if (*p==' ') { *((char*)p) = '\0'; p++; while (*p==' ') p++; }
    }

    size_t fsz = 0;
    void* data = ext2_read_entire_file(g_fs, prog, &fsz);
    if (!data) { kputs("run: cannot read file\n"); return; }
    if (!elf_validate(data)) { kputs("run: not a valid ELF64\n"); extern void kfree(void*); kfree(data); return; }

    process_t* proc = elf_load_with_args(prog, data, fsz, argc, argv_arr);
    extern void kfree(void*);
    kfree(data);
    if (!proc) { kputs("run: load failed\n"); return; }
    kputs("started process: "); kputs(proc->name); kputs("\n");
    process_switch_to(proc);
}

void cmd_scale(struct limine_framebuffer *fb, const char *args) {
    (void)fb;
    // parse unsigned int from args; default 1 if missing/invalid
    uint32_t s = 0;
    if (args) {
        while (*args == ' ') args++;
        while (*args >= '0' && *args <= '9') {
            s = s * 10 + (uint32_t)(*args - '0');
            args++;
        }
    }
    if (s == 0) s = 1;
    if (s > 16) s = 16;

    console_set_scale(s);

    print(NULL, "scale set to ");
    // tiny itoa
    char buf[12]; int i = 0; uint32_t t = s; do { buf[i++] = '0' + (t % 10); t/=10; } while (t);
    while (i--) putc_fb(NULL, buf[i]);
    print(NULL, "x\n");
}

void cmd_unknown(struct limine_framebuffer *fb, const char *cmd) {
    print(fb, "Unknown command: ");
    print(fb, cmd);
    print(fb, "\n");
    print(fb, "Type 'help' for available commands\n");
}

// ================= Command dispatch =================
typedef void (*cmd_func_t)(struct limine_framebuffer *fb);

struct command {
    const char *name;
    cmd_func_t func;
};

// Command table
struct command commands[] = {
    {"help", cmd_help},
    {"clear", cmd_clear},
    {"about", cmd_about},
    {"meminfo", cmd_meminfo},
    {"memtest", cmd_memtest},
    {"vmtest", cmd_vmtest},
    {"heaptest", cmd_heaptest},
    {"pslist", cmd_pslist},
    {"psdebug", cmd_psdebug},
    {"switch", cmd_switch},
    {"fbinfo", cmd_fbinfo},
    {"reboot",   cmd_reboot},
    {"shutdown", cmd_shutdown},
    {"pcilist", cmd_pcilist},
    {NULL, NULL} // Sentinel
};

void execute_command(struct limine_framebuffer *fb, char *input) {
    // Skip leading spaces
    while (*input == ' ') input++;
    
    // Empty command
    if (*input == '\0') return;
    
    // Find end of command word
    char *args = input;
    while (*args && *args != ' ') args++;
    
    // Split command and args
    if (*args) {
        *args = '\0'; // null terminate command
        args++; // point to arguments
        while (*args == ' ') args++; // skip spaces
    }
    
    // Special case for commands that need args
    if (strcmp(input, "echo") == 0) {
        cmd_echo(fb, args);
        return;
    }
    
    if (strcmp(input, "crash") == 0) {
        cmd_crash(fb, args);
        return;
    }

    if (strcmp(input, "ls") == 0)    { cmd_ls(fb, (args && *args) ? args : "."); return; }
    if (strcmp(input, "pwd") == 0)   { cmd_pwd(fb); return; }
    if (strcmp(input, "cd") == 0)    { cmd_cd(fb, (args && *args) ? args : "/"); return; }
    if (strcmp(input, "cat") == 0)   { cmd_cat(fb, args); return; }
    if (strcmp(input, "run") == 0)   { cmd_run(fb, args); return; }
    if (strcmp(input, "touch") == 0) { cmd_touch(fb, args); return; }

    if (strcmp(input, "append") == 0) {
        if (!args || !*args) { print(fb0(), "usage: append <path> <text>\n"); return; }
        char *p = args;
        while (*p && *p != ' ') p++;
        char *text = "";
        if (*p) { *p++ = '\0'; while (*p == ' ') p++; text = p; }
        cmd_append(fb, args, text);
        return;
    }

    if (strcmp(input, "truncate") == 0) {
        if (!args || !*args) { print(fb0(), "usage: truncate <file> <size>\n"); return; }
        char *p = args;
        while (*p && *p != ' ') p++;
        if (!*p) { print(fb0(), "usage: truncate <file> <size>\n"); return; }
        *p++ = '\0'; while (*p == ' ') p++;
        size_t n = 0; while (*p >= '0' && *p <= '9') { n = n*10 + (*p - '0'); p++; }
        cmd_truncate(fb, args, n);
        return;
    }

    

    if (strcmp(input, "scale") == 0) { cmd_scale(fb, args); return; }

        // Look up command in table
        for (int i = 0; commands[i].name != NULL; i++) {
            if (strcmp(input, commands[i].name) == 0) {
                commands[i].func(fb);
                return;
            }
        }
        
        // Command not found
        cmd_unknown(fb, input);
    }

// ================= Input handling =================
#define INPUT_BUFFER_SIZE 256

void shell_loop(struct limine_framebuffer *fb) {
    char input_buffer[INPUT_BUFFER_SIZE];
    int input_pos = 0;
    
    print(fb, "Welcome to kiwiOS!\n");
    print(fb, "Type 'help' for available commands\n\n");
    print(fb, "> ");
    
    while (1) {
        char c = keyboard_getchar();
        
        if (c == '\n') {
            // Execute command
            print(fb, "\n");
            input_buffer[input_pos] = '\0';
            
            if (input_pos > 0) {
                execute_command(fb, input_buffer);
            }
            
            // Reset for next command
            input_pos = 0;
            print(fb, "> ");
        } else if (c == '\b') {
            // Backspace
            if (input_pos > 0) {
                input_pos--;
                putc_fb(fb, '\b');
            }
        } else if (input_pos < INPUT_BUFFER_SIZE - 1) {
            // Add character to buffer
            input_buffer[input_pos++] = c;
            putc_fb(fb, c);
        }
    }
}

// ================= Kernel entry =================
void kmain(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED == false) hcf();

    if (hhdm_request.response == NULL || hhdm_request.response->offset == 0) {
        hcf();
    }
    hhdm_set_offset(hhdm_request.response->offset);

    struct limine_framebuffer *fb = fb0();
    if (!fb) hcf();

    // Disable interrupts during initialization
    asm volatile ("cli");

    display_init();

    init_idt();
    tss_init();
    gdt_init();
    syscall_init();
    acpi_init();

    if (memmap_request.response != NULL) {
        pmm_init(memmap_request.response);
    }

    vmm_init();
    heap_init();
    process_init();
    scheduler_init();

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

    // Initialize timer at 100 Hz
    timer_init(100);

    // Unmask only IRQ0 (timer)
    outb(0x21, 0xFE);

    // Enable interrupts
    asm volatile ("sti");

    bool hda_ok = hda_init();
    if (hda_ok && hda_is_present()) {
        uint16_t gcap = hda_get_gcap();
        uint8_t vmaj = hda_get_version_major();
        uint8_t vmin = hda_get_version_minor();

        print(fb0(), "HDA: controller detected, GCAP=0x");
        hx4(fb0(), gcap);
        print(fb0(), ", version ");
        print_u32(fb0(), vmaj);
        print(fb0(), ".");
        print_u32(fb0(), vmin);
        print(fb0(), "\n");

        if (hda_controller_was_reset()) {
            print(fb0(), "HDA: controller reset OK\n");
        } else {
            print(fb0(), "HDA: controller reset FAILED\n");
        }

        uint8_t hda_irq_line = hda_get_irq_line();
        if (hda_irq_line != 0xFF && hda_irq_line != 0 && hda_corb_rirb_ready()) {
            uint8_t vector = (uint8_t)(32 + hda_irq_line);
            idt_set_gate(vector, (uint64_t)irq_hda_handler);

            uint8_t master_mask = inb(0x21);
            uint8_t slave_mask  = inb(0xA1);
            if (hda_irq_line < 8) {
                master_mask &= (uint8_t)~(1u << hda_irq_line);
            } else {
                master_mask &= (uint8_t)~(1u << 2);
                slave_mask  &= (uint8_t)~(1u << (hda_irq_line - 8));
                outb(0xA1, slave_mask);
            }
            outb(0x21, master_mask);

            hda_enable_interrupts();
        }

        // NEW: try an Immediate Command to get codec 0 vendor ID
        uint32_t vendor = 0;
        if (hda_get_codec0_vendor_immediate(&vendor)) {
            print(fb0(), "HDA: codec0 vendor ID = 0x");
            // vendor ID is 16 bits in the high part of the response, but printing full 32 bits is fine for now
            hx4(fb0(), (uint16_t)(vendor >> 16));
            print(fb0(), vendor & 0xFFFF ? " (low=0x" : " (low=0x");
            hx4(fb0(), (uint16_t)(vendor & 0xFFFF));
            print(fb0(), ")\n");

            uint8_t powered_state = 0xFF;
            if (hda_codec0_set_power_state(0, HDA_POWER_STATE_D0, &powered_state)) {
                if (powered_state == HDA_POWER_STATE_D0) {
                    print(fb0(), "HDA: codec0 root entered power state D0 (fully on)\n");
                } else {
                    print(fb0(), "HDA: codec0 root reported power state D");
                    print_u32(fb0(), powered_state);
                    print(fb0(), " (expected D0)\n");
                }
            } else {
                print(fb0(), "HDA: failed to set codec0 root power state\n");
            }

            // After printing codec0 vendor, ask for its root node children
            uint8_t start_nid = 0;
            uint8_t node_count = 0;
            if (hda_codec0_get_sub_nodes(0, &start_nid, &node_count)) {
                print(fb0(), "HDA: codec0 root has ");
                print_u32(fb0(), node_count);
                print(fb0(), " nodes starting at NID ");
                print_u32(fb0(), start_nid);
                print(fb0(), "\n");
            } else {
                print(fb0(), "HDA: failed to read codec0 root node range\n");
            }

            // Now ask the Audio Function Group (NID 1) what *its* children are.
            uint8_t afg_first = 0;
            uint8_t afg_count = 0;
            if (hda_codec0_get_sub_nodes(1, &afg_first, &afg_count)) {
                print(fb0(), "HDA: AFG NID 1 has ");
                print_u32(fb0(), afg_count);
                print(fb0(), " widgets starting at NID ");
                print_u32(fb0(), afg_first);
                print(fb0(), "\n");
            } else {
                print(fb0(), "HDA: failed to read AFG (NID 1) widget range\n");
            }

            uint8_t afg_nid = 0;
            uint8_t dac_nid = 0;
            uint8_t pin_nid = 0;

            if (hda_codec0_find_output_path(&afg_nid, &dac_nid, &pin_nid)) {
                print(fb0(), "HDA: first audio output path found (AFG=");
                print_u32(fb0(), afg_nid);
                print(fb0(), ", DAC=");
                print_u32(fb0(), dac_nid);
                print(fb0(), ", PIN=");
                print_u32(fb0(), pin_nid);
                print(fb0(), ")\n");

                if (hda_codec0_power_output_path(afg_nid, dac_nid, pin_nid)) {
                    print(fb0(), "HDA: powered playback path widgets to D0\n");

                    // Configure the converter/stream binding for a simple 48kHz stereo, 16-bit path.
                    uint16_t fmt_48k_stereo_16 = 0;
                    // Channels: 2 -> (2-1) in bits [3:0]
                    fmt_48k_stereo_16 |= 1u;
                    // Bits per sample: 16-bit -> code 1 in bits [6:4]
                    fmt_48k_stereo_16 |= (1u << 4);

                    if (hda_codec0_configure_output_path(dac_nid, pin_nid, /*stream_tag=*/1, fmt_48k_stereo_16)) {
                        print(fb0(), "HDA: configured playback converter + pin (tag 1, 48kHz stereo 16-bit)\n");
                    } else {
                        print(fb0(), "HDA: failed to configure playback converter + pin\n");
                    }
                } else {
                    print(fb0(), "HDA: failed to power playback path widgets\n");
                }
            } else {
                print(fb0(), "HDA: failed to locate a DAC+output pin path on codec0\n");
            }

        } else {
            print(fb0(), "HDA: failed to read codec0 vendor ID (immediate)\n");
        }

        // after printing reset status, before or after vendor read:
        if (hda_has_codec()) {
            print(fb0(), "HDA: codec mask = 0x");
            hx4(fb0(), hda_get_codec_mask());
            print(fb0(), ", primary codec = ");
            print_u32(fb0(), hda_get_primary_codec_id());
            print(fb0(), "\n");
        } else {
            print(fb0(), "HDA: no codecs reported in STATESTS\n");
        }

        if (hda_corb_rirb_ready()) {
            print(fb0(), "HDA: CORB/RIRB init OK\n");
        } else {
            print(fb0(), "HDA: CORB/RIRB init FAILED\n");
        }


    } else {
        kputs("HDA: no HDA controller found\n");
    }

    // === Block device and disk driver initialization ===
    blockdev_init();

    int ahci_disks = ahci_init();
    if (ahci_disks > 0) {
        kputs("AHCI: ");
        print_hex(fb0(), (uint64_t)ahci_disks);
        kputs(" SATA device(s) detected\n");
    } else {
        kputs("AHCI: No devices found, falling back to ATA\n");
        ata_init();
    }

    // Initialize filesystem from first available block device
    fs_init();

    shell_loop(fb);

    hcf();
}