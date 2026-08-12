#include "serial.h"
#include "../arch/x86_64/io.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* Disable interrupts; we poll. */
    outb(COM1 + 3, 0x80); /* Enable DLAB to set the baud rate divisor. */
    outb(COM1 + 0, 0x03); /* Divisor low byte: 38400 baud. */
    outb(COM1 + 1, 0x00); /* Divisor high byte. */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit; clear DLAB. */
    outb(COM1 + 2, 0xC7); /* Enable FIFO, clear it, 14-byte threshold. */
    outb(COM1 + 4, 0x0B); /* IRQs off, RTS/DSR set. */
}

static int serial_tx_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }
    while (!serial_tx_empty()) { }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}

static void (*console_sink)(char c);

void kprintf_set_sink(void (*sink)(char c)) {
    console_sink = sink;
}

/* Every kprintf output byte goes through here so it reaches both the */
/* serial port and, once registered, the framebuffer console. */
static void kout_putc(char c) {
    serial_putc(c);
    if (console_sink != NULL) {
        console_sink(c);
    }
}

static void kout_write(const char *s) {
    while (*s) {
        kout_putc(*s++);
    }
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
        kout_putc(buf[i]);
    }
}

static void print_int(int64_t v) {
    if (v < 0) {
        kout_putc('-');
        print_uint((uint64_t)(-v), 10, 0);
    } else {
        print_uint((uint64_t)v, 10, 0);
    }
}

/* Minimal printf: %s %c %d %u %x %X %p %lx %lu %ld and %%. No width/ */
/* precision/float support -- this is a debug console, not libc. */
void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            kout_putc(*p);
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
                kout_write(s ? s : "(null)");
                break;
            }
            case 'c':
                kout_putc((char)va_arg(ap, int));
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
            case 'p':
                kout_write("0x");
                print_uint((uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0);
                break;
            case '%':
                kout_putc('%');
                break;
            case '\0':
                va_end(ap);
                return;
            default:
                kout_putc('%');
                kout_putc(*p);
                break;
        }
    }

    va_end(ap);
}
