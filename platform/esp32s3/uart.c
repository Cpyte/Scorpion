#include <stdint.h>

#include "../../platform.h"

/*
 * ESP32-S3 UART0 console.
 *
 * Same situation as the C3: the boot ROM leaves UART0 running at
 * 115200 8n1 with its default clock configuration and every dev-board
 * wires UART0 to the USB-serial bridge, so Scorpion does not touch
 * CLKDIV/CONF0 — it just polls the TX FIFO for space.
 */

#define UART0_BASE       0x60000000u

#define UART_FIFO_REG    0x000u  /* write = TX FIFO data            */
#define UART_STATUS_REG  0x01cu  /* [7:0] TXFIFO_CNT, depth = 128   */
#define UART_TXFIFO_CNT_MASK 0xFFu
#define UART_TXFIFO_DEPTH    128u

static inline volatile uint32_t *uart_reg(uint32_t off)
{
    return (volatile uint32_t *)(UART0_BASE + off);
}

void platform_console_init(void)
{
    /* Nothing to do: ROM firmware already configured UART0. */
}

void platform_uart_putc(char c)
{
    while ((*uart_reg(UART_STATUS_REG) & UART_TXFIFO_CNT_MASK) >=
           (UART_TXFIFO_DEPTH - 1u)) {
    }

    *uart_reg(UART_FIFO_REG) = (uint8_t)c;
}
