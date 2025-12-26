#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdarg.h>

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list args);
void kputs(const char *s);

#endif // LIBC_STDIO_H
