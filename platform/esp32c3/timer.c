#include <stdbool.h>
#include <stdint.h>

#include "../../scorpion.h"
#include "../../platform.h"

/*
 * ESP32-C3 system timer (SYSTIMER) as the kernel preemption source.
 *
 * The C-series chips have no CLINT-style mtime/mtimecmp: SYSTIMER is a
 * peripheral with two counters and three comparators, routed through
 * the INTC (esp_rv_intc) to a machine-level external interrupt.
 *
 * UNIT0 counts at a fixed 16 MHz derived from the PLL. We use TARGET0
 * in one-shot compare mode and re-arm it from timer_irq().
 *
 * NOTE: register offsets below follow the ESP32-C3 TRM "System Timer"
 * chapter layout shared by the v2 SYSTIMER (C3/S2/S3/H2/C6). When
 * porting, cross-check against soc/systimer_reg.h of your chip — see
 * docs/platforms.md ("Bring-up checklist").
 */

#define SYSTIMER_BASE        0x6000F000u
#define INTC_BASE            0x600C2000u

#define SYSTIMER_TARGET0_SOURCE 17u   /* ETS_SYSTIMER_TARGET0_INTR_SOURCE */
#define SYSTIMER_INT_BIT     (1u << 24)

/* SYSTIMER register offsets */
#define REG_UNIT0_VALUE_LO   0x004u
#define REG_UNIT0_VALUE_HI   0x008u
#define REG_TARGET0_HI       0x014u
#define REG_TARGET0_LO       0x018u
#define REG_COMP0_LOAD       0x034u
#define REG_UNIT0_OP         0x04Cu
#define REG_INT_CLR          0x058u
#define REG_INT_ENA          0x05Cu

/* UNIT0_OP bits */
#define UNIT0_ENABLE         (1u << 30)
#define UNIT0_UPDATE         (1u << 29)
#define UNIT0_RESET          (1u << 28)

/* INTC register offsets (esp_rv_intc) */
#define INTC_ENABLE_SET      0x004u
#define INTC_PRIORITY(n)     (0x018u + 4u * (n))
#define INTC_THRESHOLD       0x0A8u
#define INTC_EOI             0x09Cu

/* UNIT0 counts at a fixed 16 MHz; ~6.7 ms between preemptions. */
#define TIMER_INTERVAL      107500u

static volatile uint32_t *reg(uint32_t base, uint32_t off)
{
    return (volatile uint32_t *)(base + off);
}

uint64_t platform_timer_now(void)
{
    uint32_t h0, l, h1;

    do {
        h0 = *reg(SYSTIMER_BASE, REG_UNIT0_VALUE_HI);
        l  = *reg(SYSTIMER_BASE, REG_UNIT0_VALUE_LO);
        h1 = *reg(SYSTIMER_BASE, REG_UNIT0_VALUE_HI);
    } while (h0 != h1);

    return l | ((uint64_t)h1 << 32);
}

void platform_timer_arm(uint64_t deadline)
{
    /* HI first so a transient low value can never match while LO is
     * still stale. */
    *reg(SYSTIMER_BASE, REG_TARGET0_HI) = (uint32_t)(deadline >> 32);
    *reg(SYSTIMER_BASE, REG_TARGET0_LO) = (uint32_t)(deadline & 0xffffffffu);
    *reg(SYSTIMER_BASE, REG_COMP0_LOAD) = 1u;
}

bool platform_timer_is_irq(uintptr_t mcause)
{
    return mcause == PLATFORM_TIMER_MCAUSE;
}

/* Per-core part (C3 has exactly one HP core): arm comparator once and
 * unmask the interrupt in mie + INTC. */
void timer_enable(void)
{
    platform_timer_arm(platform_timer_now() + TIMER_INTERVAL);

    /* Route the SYSTIMER source through the INTC: level-triggered,
     * priority 1, threshold 0, then enable the source. */
    *reg(INTC_BASE, INTC_PRIORITY(SYSTIMER_TARGET0_SOURCE)) = 1u;
    *reg(INTC_BASE, INTC_THRESHOLD) = 0u;
    *reg(INTC_BASE, INTC_ENABLE_SET) = 1u << SYSTIMER_TARGET0_SOURCE;
    *reg(SYSTIMER_BASE, REG_INT_ENA) = SYSTIMER_INT_BIT;

    uint32_t mie;
    __asm__ volatile ("csrr %0, 0x304" : "=r"(mie));
    /* Machine external interrupt (mip bit 11). */
    mie |= (1u << 11);
    __asm__ volatile ("csrw 0x304, %0" : : "r"(mie));
}

void timer_init(void)
{
    /* Enable UNIT0 counting from zero. */
    *reg(SYSTIMER_BASE, REG_UNIT0_OP) = UNIT0_RESET;
    *reg(SYSTIMER_BASE, REG_UNIT0_OP) = UNIT0_ENABLE;

    timer_enable();
}

void timer_irq(void)
{
    platform_timer_arm(platform_timer_now() + TIMER_INTERVAL);

    /* Acknowledge at both levels: clear the SYSTIMER raw status, then
     * signal end-of-interrupt to the CPU INTC. */
    *reg(SYSTIMER_BASE, REG_INT_CLR) = SYSTIMER_INT_BIT;
    *reg(INTC_BASE, INTC_EOI) = 0u;

    if (current_process[current_core_id()] != NULL)
        yield();
}
