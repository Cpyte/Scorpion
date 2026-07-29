#include <stdarg.h>
#include <stdint.h>

#include "console.h"
#include "scorpion.h"

#define UART_BASE ((volatile uint8_t *)0x10000000)
#define UART_THR  0
#define UART_LSR  5
#define UART_LSR_THRE (1u << 5)

void console_init(void)
{
}

static void uart_putchar(char c)
{
    if (c == '\n') {
        uart_putchar('\r');
    }

    while (!(*(UART_BASE + UART_LSR) & UART_LSR_THRE)) {
    }

    *(UART_BASE + UART_THR) = (uint8_t)c;
}

void console_putchar(char c)
{
    uart_putchar(c);
}

void console_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uart_putchar(s[i]);
    }
}

void console_puts(const char *s)
{
    while (*s) {
        uart_putchar(*s++);
    }
}

static void vlog(int level, const char *fmt, va_list ap)
{
    static const char *prefix[] = {
        [LOG_DEBUG] = "[DEBUG] ",
        [LOG_INFO]  = "[INFO]  ",
        [LOG_WARN]  = "[WARN]  ",
        [LOG_ERROR] = "[ERROR] ",
        [LOG_PANIC] = "[PANIC] ",
    };

    const char *p = prefix[level];

    while (*p) {
        uart_putchar(*p++);
    }

    for (const char *s = fmt; *s; s++) {
        if (*s == '%') {
            s++;
            switch (*s) {
            case 's': {
                const char *str = va_arg(ap, const char *);
                while (*str) {
                    uart_putchar(*str++);
                }
                break;
            }
            case 'd': {
                int val = va_arg(ap, int);
                char buf[12];
                unsigned i = sizeof(buf);
                unsigned neg = 0;

                if (val < 0) {
                    neg = 1;
                    val = -val;
                }

                buf[--i] = '\0';

                do {
                    buf[--i] = (char)('0' + (val % 10));
                    val /= 10;
                } while (val);

                if (neg) {
                    buf[--i] = '-';
                }

                console_puts(&buf[i]);
                break;
            }
            case 'u': {
                unsigned val = va_arg(ap, unsigned);
                char buf[12];
                unsigned i = sizeof(buf);

                buf[--i] = '\0';

                do {
                    buf[--i] = (char)('0' + (val % 10));
                    val /= 10;
                } while (val);

                console_puts(&buf[i]);
                break;
            }
            case 'x': {
                unsigned val = va_arg(ap, unsigned);
                char buf[12];
                unsigned i = sizeof(buf);

                buf[--i] = '\0';

                do {
                    unsigned d = val & 0xF;
                    buf[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                    val >>= 4;
                } while (val);

                console_puts(&buf[i]);
                break;
            }
            case 'p': {
                uintptr_t val = va_arg(ap, uintptr_t);
                console_puts("0x");

                for (int j = sizeof(uintptr_t) * 2 - 1; j >= 0; j--) {
                    unsigned d = (val >> (j * 4)) & 0xF;
                    uart_putchar((char)(d < 10 ? '0' + d : 'a' + d - 10));
                }
                break;
            }
            default:
                uart_putchar(*s);
                break;
            }
        } else {
            uart_putchar(*s);
        }
    }

    uart_putchar('\n');
}

void log_message(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(level, fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(LOG_INFO, fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(LOG_WARN, fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(LOG_ERROR, fmt, ap);
    va_end(ap);
}

void __attribute__((noreturn)) panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vlog(LOG_PANIC, fmt, ap);
    va_end(ap);

    console_puts("--- system halted ---\n");

    for (;;) {
        __asm__ volatile ("wfi");
    }
}
