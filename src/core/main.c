#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch/x86/gdt.h"
#include "limine.h"
#include "font8x16_tandy2k.h"
#include "memory/pmm.h"
#include "arch/x86/tss.h"
#include "memory/vmm.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include "arch/x86/io.h"
#include "lib/string.h"

static void kputs(const char* s);


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
#define GLYPH_H 16

static struct limine_framebuffer *g_fbs[MAX_OUTPUTS];
static uint32_t g_fb_count = 0;

// Text layout bounds shared by all outputs (min width/height across displays)
static uint32_t g_text_w_px = 0;  // usable width in pixels (min across outputs)
static uint32_t g_text_h_px = 0;  // usable height in pixels (min across outputs)

// Forward declarations for helpers used during display initialization.
static void update_layout_from_bounds(void);
static void reset_scrollback(void);
static void clear_outputs(void);
static void render_visible(void);

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

    update_layout_from_bounds();
    reset_scrollback();
    clear_outputs();
    render_visible();
}

// ================= Text renderer (mirrored to all outputs) =================
// Fast scrolling backed by a scrollback buffer and an 8x16 Tandy 2000 font.

static const uint32_t DEFAULT_FG = 0x00C0C0C0; // light gray
static const uint32_t DEFAULT_BG = 0x00000000; // black

static uint32_t fg_color = 0x00C0C0C0;
static uint32_t bg_color = 0x00000000;

// Integer text scale (1=normal, 2=double, ...)
static uint32_t g_scale = 1;
static inline uint32_t CELL_W(void){ return GLYPH_W * g_scale; }
static inline uint32_t CELL_H(void){ return GLYPH_H * g_scale; }

static inline void fill_row_span(uint8_t *row_base, uint32_t pixels, uint32_t color) {
    uint32_t *p = (uint32_t *)row_base;
    for (uint32_t x = 0; x < pixels; x++) p[x] = color;
}

// Basic ANSI color palette (0-7 normal, 8-15 bright)
static const uint32_t ansi_palette[16] = {
    0x00000000, // 0 black
    0x00AA0000, // 1 red
    0x0000AA00, // 2 green
    0x00AA5500, // 3 yellow/brown
    0x000000AA, // 4 blue
    0x00AA00AA, // 5 magenta
    0x0000AAAA, // 6 cyan
    0x00AAAAAA, // 7 light gray
    0x00555555, // 8 dark gray
    0x00FF5555, // 9 bright red
    0x0055FF55, // 10 bright green
    0x00FFFF55, // 11 bright yellow
    0x005555FF, // 12 bright blue
    0x00FF55FF, // 13 bright magenta
    0x0055FFFF, // 14 bright cyan
    0x00FFFFFF  // 15 white
};

static void ansi_reset_state(void);

// Scrollback buffer ------------------------------------------------------

#define MAX_COLS          512
#define SCROLLBACK_LINES 1024

struct cell { char ch; uint32_t fg; uint32_t bg; };
static struct cell g_buffer[SCROLLBACK_LINES][MAX_COLS];

static uint32_t g_cols = 0;               // columns in the visible area
static uint32_t g_rows = 0;               // rows in the visible area
static uint32_t g_head = 0;               // logical line 0 -> g_buffer[g_head]
static uint32_t g_line_count = 0;         // number of valid lines in buffer
static uint32_t g_view_offset = 0;        // how many lines up from the newest view is
static uint32_t g_cursor_col = 0;         // cursor column within the newest line

static inline uint32_t wrap_line(uint32_t logical) {
    return (g_head + logical) % SCROLLBACK_LINES;
}

static void clear_line(uint32_t logical_line) {
    uint32_t idx = wrap_line(logical_line);
    for (uint32_t x = 0; x < g_cols && x < MAX_COLS; x++) {
        g_buffer[idx][x].ch = ' ';
        g_buffer[idx][x].fg = fg_color;
        g_buffer[idx][x].bg = bg_color;
    }
}

static void reset_scrollback(void) {
    fg_color = DEFAULT_FG;
    bg_color = DEFAULT_BG;
    ansi_reset_state();

    g_head = 0;
    g_line_count = 1;
    g_view_offset = 0;
    g_cursor_col = 0;
    clear_line(0);
}

static void update_layout_from_bounds(void) {
    if (CELL_W() == 0 || CELL_H() == 0) return;

    g_cols = g_text_w_px / CELL_W();
    if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    if (g_cols == 0) g_cols = 1;
    g_text_w_px = g_cols * CELL_W();

    g_rows = g_text_h_px / CELL_H();
    if (g_rows == 0) g_rows = 1;
    g_text_h_px = g_rows * CELL_H();
}

static uint32_t max_view_offset(void) {
    if (g_line_count <= g_rows) return 0;
    return g_line_count - g_rows;
}

static uint32_t view_start_line(void) {
    uint32_t max_off = max_view_offset();
    if (g_view_offset > max_off) g_view_offset = max_off;
    if (g_line_count <= g_rows) return 0;
    return g_line_count - g_rows - g_view_offset;
}

static void clear_outputs(void) {
    for (uint32_t i = 0; i < g_fb_count; i++) {
        struct limine_framebuffer *out = g_fbs[i];
        uint8_t *base = (uint8_t *)(uintptr_t)out->address;
        size_t pitch = (size_t)out->pitch;
        for (uint32_t y = 0; y < g_text_h_px; y++) {
            uint8_t *row = base + (size_t)y * pitch;
            fill_row_span(row, g_text_w_px, bg_color);
        }
    }
}

// Font blitting ----------------------------------------------------------
static void draw_char_scaled(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font8x16_tandy2k[(uint8_t)c];

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

static void draw_cell(uint32_t view_row, uint32_t col, const struct cell *c) {
    draw_char_scaled(col * CELL_W(), view_row * CELL_H(), c->ch, c->fg, c->bg);
}

static void draw_blank_cell(uint32_t view_row, uint32_t col) {
    struct cell blank = { ' ', fg_color, bg_color };
    draw_cell(view_row, col, &blank);
}

static void render_line_to_row(uint32_t logical_line, uint32_t view_row) {
    uint32_t idx = wrap_line(logical_line);
    const struct cell *line = g_buffer[idx];
    for (uint32_t col = 0; col < g_cols; col++) {
        draw_cell(view_row, col, &line[col]);
    }
}

static void render_visible(void) {
    uint32_t start = view_start_line();
    for (uint32_t row = 0; row < g_rows; row++) {
        uint32_t logical = start + row;
        if (logical < g_line_count) {
            render_line_to_row(logical, row);
        } else {
            for (uint32_t col = 0; col < g_cols; col++) draw_blank_cell(row, col);
        }
    }
}

static void scroll_view_up_one(void) {
    const uint32_t step = CELL_H();
    if (step == 0 || g_text_h_px < step) return;

    for (uint32_t i = 0; i < g_fb_count; i++) {
        struct limine_framebuffer *out = g_fbs[i];
        uint8_t *base   = (uint8_t *)(uintptr_t)out->address;
        size_t   pitch  = (size_t)out->pitch;

        for (uint32_t y = 0; y + step < g_text_h_px; y++) {
            uint8_t *dest = base + (size_t)y * pitch;
            uint8_t *src  = dest + (size_t)step * pitch;
            memmove(dest, src, (size_t)g_text_w_px * 4);
        }

        for (uint32_t y = g_text_h_px - step; y < g_text_h_px; y++) {
            uint8_t *row = base + (size_t)y * pitch;
            fill_row_span(row, g_text_w_px, bg_color);
        }
    }
}

static void new_line(void) {
    if (g_line_count < SCROLLBACK_LINES) {
        clear_line(g_line_count);
        g_line_count++;
    } else {
        g_head = (g_head + 1) % SCROLLBACK_LINES;
        clear_line(g_line_count - 1);
    }

    g_cursor_col = 0;

    if (g_view_offset == 0) {
        if (g_line_count > g_rows) {
            scroll_view_up_one();
            render_line_to_row(g_line_count - 1, g_rows - 1);
        } else {
            render_visible();
        }
    } else {
        (void)view_start_line();
    }
}

static void console_page_up(void) {
    uint32_t max_off = max_view_offset();
    if (max_off == 0) return;

    uint32_t step = (g_rows > 1) ? (g_rows - 1) : 1;
    if (g_view_offset + step > max_off) step = max_off - g_view_offset;
    g_view_offset += step;
    render_visible();
}

static void console_page_down(void) {
    if (g_view_offset == 0) return;

    uint32_t step = (g_rows > 1) ? (g_rows - 1) : 1;
    if (step > g_view_offset) step = g_view_offset;
    g_view_offset -= step;
    render_visible();
}

// Public: allow shell to change scale
void console_set_scale(uint32_t new_scale) {
    if (new_scale == 0) new_scale = 1;
    if (new_scale > 16) new_scale = 16;
    if (new_scale == g_scale) return;

    g_scale = new_scale;

    update_layout_from_bounds();
    clear_outputs();
    reset_scrollback();
    render_visible();
}

// --- Required exports (same names as your existing code) ---

void scroll_up(struct limine_framebuffer *fb /*unused*/) {
    (void)fb;
    new_line();
}

void draw_char(struct limine_framebuffer *fb /*unused*/,
               uint32_t x, uint32_t y,
               char c, uint32_t fg, uint32_t bg) {
    (void)fb;
    draw_char_scaled(x, y, c, fg, bg);
}

// ANSI escape parsing state for simple color control
static enum { ANSI_NORMAL, ANSI_ESC, ANSI_CSI } ansi_state = ANSI_NORMAL;
static uint32_t ansi_params[8];
static uint32_t ansi_param_count = 0;

static void ansi_reset_state(void) {
    ansi_state = ANSI_NORMAL;
    ansi_param_count = 0;
    for (uint32_t i = 0; i < 8; i++) ansi_params[i] = 0;
}

static void apply_sgr_params(void) {
    if (ansi_param_count == 0) {
        fg_color = DEFAULT_FG;
        bg_color = DEFAULT_BG;
        return;
    }

    for (uint32_t i = 0; i < ansi_param_count; i++) {
        uint32_t p = ansi_params[i];
        if (p == 0) {
            fg_color = DEFAULT_FG;
            bg_color = DEFAULT_BG;
        } else if (p == 39) {
            fg_color = DEFAULT_FG;
        } else if (p == 49) {
            bg_color = DEFAULT_BG;
        } else if (p >= 30 && p <= 37) {
            fg_color = ansi_palette[p - 30];
        } else if (p >= 90 && p <= 97) {
            fg_color = ansi_palette[(p - 90) + 8];
        } else if (p >= 40 && p <= 47) {
            bg_color = ansi_palette[p - 40];
        } else if (p >= 100 && p <= 107) {
            bg_color = ansi_palette[(p - 100) + 8];
        }
    }
}

// Draw char at cursor (advances cursor) — mirrored to all outputs
void putc_fb(struct limine_framebuffer *fb /*unused*/, char c) {
    (void)fb;

    if (ansi_state == ANSI_ESC) {
        if (c == '[') {
            ansi_state = ANSI_CSI;
            ansi_param_count = 0;
            ansi_params[0] = 0;
        } else {
            ansi_reset_state();
        }
        return;
    } else if (ansi_state == ANSI_CSI) {
        if (c >= '0' && c <= '9') {
            ansi_params[ansi_param_count] = ansi_params[ansi_param_count] * 10 + (uint32_t)(c - '0');
        } else if (c == ';') {
            if (ansi_param_count + 1 < 8) {
                ansi_param_count++;
                ansi_params[ansi_param_count] = 0;
            }
        } else {
            ansi_param_count++;
            if (c == 'm') {
                apply_sgr_params();
            }
            ansi_reset_state();
        }
        return;
    }

    if (c == '\x1B') { ansi_state = ANSI_ESC; return; }
    if (c == '\n') { new_line(); return; }

    if (c == '\b') {
        if (g_cursor_col > 0) {
            g_cursor_col--;
            uint32_t logical_line = g_line_count - 1;
            g_buffer[wrap_line(logical_line)][g_cursor_col].ch = ' ';
            g_buffer[wrap_line(logical_line)][g_cursor_col].fg = fg_color;
            g_buffer[wrap_line(logical_line)][g_cursor_col].bg = bg_color;

            uint32_t start = view_start_line();
            if (g_view_offset <= max_view_offset() && logical_line >= start && logical_line < start + g_rows) {
                render_line_to_row(logical_line, logical_line - start);
            }
        }
        return;
    }

    if (g_cursor_col >= g_cols) new_line();

    uint32_t logical_line = g_line_count - 1;
    uint32_t idx = wrap_line(logical_line);
    g_buffer[idx][g_cursor_col].ch = c;
    g_buffer[idx][g_cursor_col].fg = fg_color;
    g_buffer[idx][g_cursor_col].bg = bg_color;

    uint32_t start = view_start_line();
    if (logical_line >= start && logical_line < start + g_rows && g_cursor_col < g_cols) {
        draw_cell(logical_line - start, g_cursor_col, &g_buffer[idx][g_cursor_col]);
    }

    g_cursor_col++;
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

    // --- Kernel-mode fault: collect extra context before drawing ---
    uint64_t cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    uint32_t old_fg = fg_color, old_bg = bg_color;

    // Reset scrollback first (it restores DEFAULT colors), then set
    // the panic colors so they are used for clearing and rendering.
    reset_scrollback();
    fg_color = 0x00FFFFFF;
    bg_color = 0x00913030;

    // Ensure the scrollback buffer's visible lines use the panic bg
    // (reset_scrollback wrote DEFAULT_BG into the cells). Clear each
    // visible logical line so `render_visible` won't draw the old
    // default background over our red clear.
    for (uint32_t i = 0; i < g_rows; i++) clear_line(i);

    clear_outputs();
    render_visible();

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
static bool e0_prefix     = false;
static int  pending_special = -1;

enum {
    KEY_ARROW_UP   = -16,
    KEY_ARROW_DOWN = -17,
};

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

static bool handle_extended_scancode(uint8_t scancode) {
    if (scancode & 0x80) return true; // ignore extended releases

    switch (scancode) {
        case 0x49: console_page_up();   return true;
        case 0x51: console_page_down(); return true;
        case 0x48: pending_special = KEY_ARROW_UP;   return true; // Up arrow
        case 0x50: pending_special = KEY_ARROW_DOWN; return true; // Down arrow
        default:   return false;
    }
}

char keyboard_getchar(void) {
    if (pending_special != -1) {
        char k = (char)pending_special;
        pending_special = -1;
        return k;
    }

    for (;;) {
        // Data available?
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & 0x01)) continue;

        uint8_t scancode = inb(PS2_DATA_PORT);

        if (scancode == 0xE0) { e0_prefix = true; continue; }
        if (e0_prefix) {
            bool consumed = handle_extended_scancode(scancode);
            e0_prefix = false;
            if (consumed) {
                if (pending_special != -1) {
                    char k = (char)pending_special;
                    pending_special = -1;
                    return k;
                }
                continue;
            }
        }

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
    if (pending_special != -1) {
        char k = (char)pending_special;
        pending_special = -1;
        return (int)k;
    }

    // Any data?
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & 0x01)) return -1;

    uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode == 0xE0) { e0_prefix = true; return -1; }
    if (e0_prefix) {
        bool consumed = handle_extended_scancode(scancode);
        e0_prefix = false;
        if (consumed) {
            if (pending_special != -1) {
                char k = (char)pending_special;
                pending_special = -1;
                return (int)k;
            }
            return -1;
        }
    }

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
    print(fb, "  clear      - Clear the console\n");
    print(fb, "  echo [msg] - Print a message\n");
    print(fb, "  about      - Show information about KiwiOS\n");
    print(fb, "  crash [n]  - Trigger exception number n\n");
    print(fb, "  meminfo    - Show memory usage information\n");
    print(fb, "  memtest    - Run a memory test\n");
    print(fb, "  vmtest     - Run a VMM test\n");
    print(fb, "  heaptest   - Run a heap allocation test\n");
    print(fb, "  scale [factor] - Set framebuffer scaling factor\n");
}

// lightweight print helpers
static void kputs(const char* s) { print(fb0(), s); }

void cmd_clear(struct limine_framebuffer *fb /*unused*/) {
    (void)fb;
    clear_outputs();
    reset_scrollback();
    render_visible();
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
    if (s > 16) s = 9;

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
    {"fbinfo", cmd_fbinfo},
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

#define HISTORY_SIZE 32
static char history[HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int history_count = 0;   // number of stored entries
static int history_cursor = -1; // -1 = live typing, 0 = newest history, ...
static char history_scratch[INPUT_BUFFER_SIZE];
static int history_scratch_len = 0;

static void history_record(const char *line) {
    if (!line || !*line) return;

    // Avoid duplicate consecutive entries
    if (history_count > 0) {
        const char *last = history[(history_count - 1) % HISTORY_SIZE];
        if (strncmp(last, line, INPUT_BUFFER_SIZE) == 0) return;
    }

    size_t len = strlen(line);
    if (len >= INPUT_BUFFER_SIZE) len = INPUT_BUFFER_SIZE - 1;

    int slot = history_count % HISTORY_SIZE;
    memcpy(history[slot], line, len);
    history[slot][len] = '\0';
    history_count++;
}

static void reset_history_navigation(void) {
    history_cursor = -1;
    history_scratch_len = 0;
}

static const char *history_fetch(int cursor_from_newest) {
    if (cursor_from_newest < 0) return NULL;
    if (cursor_from_newest >= history_count) return NULL;
    int logical = history_count - 1 - cursor_from_newest;
    return history[logical % HISTORY_SIZE];
}

static void replace_input_line(struct limine_framebuffer *fb,
                               char *buffer, int *pos,
                               const char *text) {
    while (*pos > 0) {
        putc_fb(fb, '\b');
        (*pos)--;
    }

    const char *p = text;
    while (*p && *pos < INPUT_BUFFER_SIZE - 1) {
        buffer[*pos] = *p;
        putc_fb(fb, *p);
        (*pos)++;
        p++;
    }
}

void shell_loop(struct limine_framebuffer *fb) {
    char input_buffer[INPUT_BUFFER_SIZE];
    int input_pos = 0;

    print(fb, "Welcome to kiwiOS!\n");
    print(fb, "Type 'help' for available commands\n\n");
    print(fb, "> ");
    
    while (1) {
        char c = keyboard_getchar();
            if (c == KEY_ARROW_UP) {
            if (history_cursor == -1) {
                history_scratch_len = input_pos;
                if (history_scratch_len > INPUT_BUFFER_SIZE - 1) history_scratch_len = INPUT_BUFFER_SIZE - 1;
                memcpy(history_scratch, input_buffer, (size_t)history_scratch_len);
                history_scratch[history_scratch_len] = '\0';
            }

            if (history_cursor + 1 < history_count) {
                history_cursor++;
                const char *entry = history_fetch(history_cursor);
                if (entry) replace_input_line(fb, input_buffer, &input_pos, entry);
            }
            continue;
        }

        if (c == KEY_ARROW_DOWN) {
            if (history_cursor > 0) {
                history_cursor--;
                const char *entry = history_fetch(history_cursor);
                if (entry) replace_input_line(fb, input_buffer, &input_pos, entry);
            } else if (history_cursor == 0) {
                history_cursor = -1;
                replace_input_line(fb, input_buffer, &input_pos, history_scratch);
            }
            continue;
        }

        if (c == '\n') {
            // Execute command
            print(fb, "\n");
            input_buffer[input_pos] = '\0';
            
            if (input_pos > 0) {
                history_record(input_buffer);
                execute_command(fb, input_buffer);
            }
            
            // Reset for next command
            input_pos = 0;
            print(fb, "> ");
            reset_history_navigation();
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

// Enable x86_64 FPU/SSE for both kernel and userspace.
// x86_64 codegen assumes at least SSE2 is available; if CR0/CR4 aren't set up
// properly, the first XMM instruction in ring 3 will #UD and appear as a "freeze".
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
    x86_enable_sse();

    if (memmap_request.response != NULL) {
        pmm_init(memmap_request.response);
    }

    vmm_init();
    heap_init();

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

    // Enable interrupts
    asm volatile ("sti");

    shell_loop(fb);
    
    // We should never return here; halt if we do.
    while (1) { asm volatile ("hlt"); }
}
