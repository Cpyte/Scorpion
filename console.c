#include <stdarg.h>
#include <stdint.h>

#include "console.h"
#include "platform.h"
#include "scorpion.h"

/*
 * Console formatting layer. All hardware access goes through the
 * platform hooks declared in platform.h (see platform/<name>/uart.c).
 *
 * Kernel diagnostics go through a line buffer and are written to the
 * UART as a contiguous burst when the line ends (or the buffer fills),
 * instead of polling the TX FIFO once per character. User-facing
 * console_write (the PUTC syscall) stays synchronous so the caller's
 * bytes reach the wire before the call returns.
 *
 * Every output path masks interrupts for its critical section. This
 * keeps a console-lock holder from being preempted on the same core
 * into a path that needs another lock acquired while the console lock
 * is held (e.g. the scheduler queue lock via a timer-ISR yield), which
 * would deadlock the SMP pair.
 */

#define CONSOLE_BUF_MAX 128

static char console_buf[CONSOLE_BUF_MAX];
static unsigned console_buf_len;
static spinlock_t console_lock;

static void uart_putchar(char c)
{
    if (c == '\n') {
        platform_uart_putc('\r');
    }

    platform_uart_putc(c);
}

static void flush_locked(void)
{
    for (unsigned i = 0; i < console_buf_len; i++) {
        uart_putchar(console_buf[i]);
    }
    console_buf_len = 0;
}

static void putchar_buffered(char c)
{
    if (console_buf_len >= CONSOLE_BUF_MAX) {
        flush_locked();
    }
    console_buf[console_buf_len++] = c;
    if (c == '\n') {
        flush_locked();
    }
}

void console_flush(void)
{
    uint32_t flags;

    flags = irq_save();
    spinlock_lock(&console_lock);
    flush_locked();
    spinlock_unlock(&console_lock);
    irq_restore(flags);
}

void console_init(void)
{
    spinlock_init(&console_lock);
    platform_console_init();
}

void console_putchar(char c)
{
    uint32_t flags;

    flags = irq_save();
    spinlock_lock(&console_lock);
    putchar_buffered(c);
    spinlock_unlock(&console_lock);
    irq_restore(flags);
}

void console_write(const char *s, size_t len)
{
    uint32_t flags;

    flags = irq_save();
    spinlock_lock(&console_lock);
    flush_locked();
    for (size_t i = 0; i < len; i++) {
        uart_putchar(s[i]);
    }
    spinlock_unlock(&console_lock);
    irq_restore(flags);
}

void console_puts(const char *s)
{
    uint32_t flags;

    flags = irq_save();
    spinlock_lock(&console_lock);
    while (*s) {
        putchar_buffered(*s++);
    }
    spinlock_unlock(&console_lock);
    irq_restore(flags);
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
        console_putchar(*p++);
    }

    for (const char *s = fmt; *s; s++) {
        if (*s == '%') {
            s++;
            switch (*s) {
            case 's': {
                const char *str = va_arg(ap, const char *);
                while (*str) {
                    console_putchar(*str++);
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
                    console_putchar((char)(d < 10 ? '0' + d : 'a' + d - 10));
                }
                break;
            }
            default:
                console_putchar(*s);
                break;
            }
        } else {
            console_putchar(*s);
        }
    }

    console_putchar('\n');
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
    console_flush();

    for (;;) {
        ARCH_IDLE();
    }
}