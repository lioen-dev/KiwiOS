#ifndef CORE_CONSOLE_H
#define CORE_CONSOLE_H

#include <stdint.h>
#include "limine.h"

void console_init(void);
struct limine_framebuffer *console_primary_framebuffer(void);

void console_clear(void);
void console_reset_scrollback(void);
void console_clear_outputs(void);
void console_render_visible(void);

void console_set_colors(uint32_t fg, uint32_t bg);
void console_get_colors(uint32_t *fg, uint32_t *bg);

void console_page_up(void);
void console_page_down(void);
void console_set_scale(uint32_t scale);

void putc_fb(struct limine_framebuffer *fb, char c);
void print(struct limine_framebuffer *fb, const char *s);
void print_hex(struct limine_framebuffer *fb, uint64_t num);
void print_u64(struct limine_framebuffer *fb, uint64_t v);
void print_u32(struct limine_framebuffer *fb, uint32_t v);

#endif // CORE_CONSOLE_H
