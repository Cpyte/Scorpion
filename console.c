#include <stdarg.h>
#include <stdint.h>

#include "console.h"
#include "scorpion.h"

#define UART_BASE    0x40070000u
#define UART_DR      0x000
#define UART_FR      0x018
#define UART_IBRD    0x024
#define UART_FBRD    0x028
#define UART_LCR_H   0x02c
#define UART_CR      0x030
#define UART_FR_TXFF (1u << 5)
#define UART_FR_BUSY (1u << 3)

#define IO_BANK0_BASE   0x40028000u
#define GPIO0_CTRL      (IO_BANK0_BASE + 0x004)
#define GPIO1_CTRL      (IO_BANK0_BASE + 0x00c)
#define IO_FUNCSEL_MASK 0x1f

#define SYS_CLK_HZ      150000000u

static volatile uint32_t *reg32(unsigned addr)
{
    return (volatile uint32_t *)addr;
}

void console_init(void)
{
    /* Route GPIO0 → UART0 TX (func 2), GPIO1 → UART0 RX (func 2) */
    *reg32(GPIO0_CTRL) = (*reg32(GPIO0_CTRL) & ~IO_FUNCSEL_MASK) | 2;
    *reg32(GPIO1_CTRL) = (*reg32(GPIO1_CTRL) & ~IO_FUNCSEL_MASK) | 2;

    volatile uint32_t *uart = (volatile uint32_t *)UART_BASE;

    /* Disable UART while configuring */
    uart[UART_CR / 4] = 0;

    /* PL011 baud: IBRD = clk/(16*baud), FBRD = fractional part × 64 */
    uint32_t baud_div = (8u * SYS_CLK_HZ / 115200u) + 1;
    uint32_t ibrd = baud_div >> 7;
    uint32_t fbrd = (baud_div & 0x7fu) >> 1;

    uart[UART_IBRD / 4] = ibrd;
    uart[UART_FBRD / 4] = fbrd;

    /* 8n1, FIFOs enabled */
    uart[UART_LCR_H / 4] = 0x70;      /* WLEN=3 (8-bit), FEN=1 (FIFO) */

    /* Enable UART, TX, RX */
    uart[UART_CR / 4]  = 0x301;       /* UARTEN | TXE | RXE */
}

static void uart_putchar(char c)
{
    if (c == '\n') {
        uart_putchar('\r');
    }

    while ((*(volatile uint16_t *)(UART_BASE + UART_FR) & UART_FR_TXFF)) {
    }

    *(volatile uint32_t *)(UART_BASE + UART_DR) = (uint8_t)c;
}

void console_putchar(char c)
{
    uart_putchar(c);
}

// Man, pieces of software these days are full of potatoes!

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
