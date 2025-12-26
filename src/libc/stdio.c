#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "core/console.h"
#include "libc/stdio.h"

static void print_unsigned(unsigned long long value, int base, bool uppercase) {
    char buf[32];
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int pos = 0;

    if (value == 0) {
        putc_fb(NULL, '0');
        return;
    }

    while (value && pos < (int)(sizeof(buf))) {
        buf[pos++] = digits[value % (unsigned)base];
        value /= (unsigned)base;
    }

    while (pos--) {
        putc_fb(NULL, buf[pos]);
    }
}

static void print_signed(long long value) {
    if (value < 0) {
        putc_fb(NULL, '-');
        print_unsigned((unsigned long long)(-value), 10, false);
    } else {
        print_unsigned((unsigned long long)value, 10, false);
    }
}

void kvprintf(const char *fmt, va_list args) {
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            putc_fb(NULL, *p);
            continue;
        }

        ++p;
        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (s) print(NULL, s);
                else print(NULL, "(null)");
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                putc_fb(NULL, c);
                break;
            }
            case 'd':
            case 'i': {
                int val = va_arg(args, int);
                print_signed(val);
                break;
            }
            case 'u': {
                unsigned int val = va_arg(args, unsigned int);
                print_unsigned(val, 10, false);
                break;
            }
            case 'x': {
                unsigned int val = va_arg(args, unsigned int);
                print_unsigned(val, 16, false);
                break;
            }
            case 'X': {
        unsigned int val = va_arg(args, unsigned int);
        print_unsigned(val, 16, true);
        break;
    }
    case 'p': {
        uint64_t val = (uint64_t)(uintptr_t)va_arg(args, void *);
        print_hex(NULL, val);
        break;
    }
            case '%': {
                putc_fb(NULL, '%');
                break;
            }
            default:
                putc_fb(NULL, '%');
                putc_fb(NULL, *p);
                break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}

void kputs(const char *s) {
    print(NULL, s);
    putc_fb(NULL, '\n');
}
