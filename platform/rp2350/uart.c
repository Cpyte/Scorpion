#include <stdint.h>

#include "platform.h"

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

void platform_console_init(void)
{
    /* Route GPIO0 -> UART0 TX (func 2), GPIO1 -> UART0 RX (func 2) */
    *reg32(GPIO0_CTRL) = (*reg32(GPIO0_CTRL) & ~IO_FUNCSEL_MASK) | 2;
    *reg32(GPIO1_CTRL) = (*reg32(GPIO1_CTRL) & ~IO_FUNCSEL_MASK) | 2;

    volatile uint32_t *uart = (volatile uint32_t *)UART_BASE;

    /* Disable UART while configuring */
    uart[UART_CR / 4] = 0;

    /* PL011 baud: IBRD = clk/(16*baud), FBRD = fractional part x 64 */
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

void platform_uart_putc(char c)
{
    while ((*(volatile uint16_t *)(UART_BASE + UART_FR) & UART_FR_TXFF)) {
    }

    *(volatile uint32_t *)(UART_BASE + UART_DR) = (uint8_t)c;
}
