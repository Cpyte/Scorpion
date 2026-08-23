#include <stdbool.h>
#include <stdint.h>

#include "../../scorpion.h"
#include "../../platform.h"

/*
 * ESP32-S3 system timer (SYSTIMER) as the kernel preemption source.
 *
 * Same SYSTIMER IP block as the C3 (UNIT0 @16 MHz, TARGET0 one-shot
 * compare), but on Xtensa there is no machine-mode interrupt controller
 * to route through: peripheral sources are steered to CPU interrupts by
 * the interrupt MATRIX, and delivery is controlled by the INTENABLE /
 * INTERRUPT special registers.
 *
 * Scorpion uses CPU interrupt 2 (priority level 1) exclusively:
 *
 *   SYSTIMER_TARGET0 (source 57) --matrix--> CPU int 2 --> level-1 IRQ
 *
 * The trap entry assembly synthesizes mcause 0x80000000|2 for it, which
 * PLATFORM_TIMER_MCAUSE mirrors.
 *
 * NOTE: SYSTIMER register offsets follow the TRM "System Timer" chapter
 * shared by C3/S2/S3/H2/C6; cross-check soc/systimer_reg.h when porting
 * further (docs/platforms.md, "Bring-up checklist").
 */

#define SYSTIMER_BASE        0x6000F000u

#define ETS_SYSTIMER_TARGET0_INTR_SOURCE 57u
#define XT_CPU_INT           2u              /* priority level 1        */
#define XT_INT_BIT           (1u << XT_CPU_INT)

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

/* UNIT0 counts at a fixed 16 MHz; ~6.7 ms between preemptions. */
#define TIMER_INTERVAL      107500u

/* ROM helper (esp32s3.rom.ld): route a peripheral source to a CPU
 * interrupt number. Signature per ROM: void intr_matrix_set(int cpu,
 * uint32_t src, uint32_t cpu_int). */
#define ROM_INTR_MATRIX_SET  0x40001b54u

typedef void (*rom_intr_matrix_set_fn)(int cpu, uint32_t src, uint32_t intr);

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

void timer_enable(void)
{
    static rom_intr_matrix_set_fn rom_intr_matrix_set =
        (rom_intr_matrix_set_fn)(uintptr_t)ROM_INTR_MATRIX_SET;

    platform_timer_arm(platform_timer_now() + TIMER_INTERVAL);

    /* Route source -> CPU int 2, unmask it in the SYSTIMER and in the
     * CPU's INTENABLE (bit 2). */
    rom_intr_matrix_set(0, ETS_SYSTIMER_TARGET0_INTR_SOURCE, XT_CPU_INT);
    *reg(SYSTIMER_BASE, REG_INT_ENA) = (1u << 24);   /* target0 edge/level */

    uint32_t intenable;
    __asm__ volatile ("rsr.intenable %0" : "=r"(intenable));
    intenable |= XT_INT_BIT;
    __asm__ volatile ("wsr.intenable %0; rsync" : : "r"(intenable));
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

    /* Acknowledge at both levels: clear the SYSTIMER raw status bit,
     * then clear the CPU-level latch in INTERRUPT via INTCLEAR. */
    *reg(SYSTIMER_BASE, REG_INT_CLR) = (1u << 24);

    uint32_t intclear = XT_INT_BIT;
    __asm__ volatile ("wsr.intclear %0" : : "r"(intclear));

    if (current_process[current_core_id()] != NULL)
        yield();
}
