/* Re-implements kprintf's exact minimal format-spec support (see
 * drivers/serial.h/.c) over write(1, ...), since userland can't call
 * kprintf directly. Same intentional gaps: no width/precision. */

#include "../libc.h"

#include <stdarg.h>
#include <stdint.h>

void putchar(char c) {
    write(1, &c, 1);
}

void puts(const char *s) {
    write(1, s, strlen(s));
    putchar('\n');
}

static void print_uint(uint64_t v, unsigned base, int upper) {
    static const char lower[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower;
    char buf[32];
    int i = 0;

    if (v == 0) {
        buf[i++] = '0';
    }
    while (v) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i--) {
        putchar(buf[i]);
    }
}

static void print_int(int64_t v) {
    if (v < 0) {
        putchar('-');
        print_uint((uint64_t)(-v), 10, 0);
    } else {
        print_uint((uint64_t)v, 10, 0);
    }
}

void printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            putchar(*p);
            continue;
        }

        p++;
        int is_long = 0;
        if (*p == 'l') {
            is_long = 1;
            p++;
        }

        switch (*p) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                const char *out = s ? s : "(null)";
                write(1, out, strlen(out));
                break;
            }
            case 'c':
                putchar((char)va_arg(ap, int));
                break;
            case 'd':
                print_int(is_long ? va_arg(ap, int64_t) : va_arg(ap, int));
                break;
            case 'u':
                print_uint(is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int), 10, 0);
                break;
            case 'x':
                print_uint(is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int), 16, 0);
                break;
            case 'X':
                print_uint(is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int), 16, 1);
                break;
            case 'p': {
                const char *hex = "0x";
                write(1, hex, 2);
                print_uint((uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0);
                break;
            }
            case '%':
                putchar('%');
                break;
            case '\0':
                va_end(ap);
                return;
            default:
                putchar('%');
                putchar(*p);
                break;
        }
    }

    va_end(ap);
}
