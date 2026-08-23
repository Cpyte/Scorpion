#include <stdbool.h>
#include <stdint.h>

#include "scorpion.h"
#include "platform.h"

/*
 * RP2350 machine timer: a RISC-V platform timer lives in the SIO block
 * (0xd0000000). One shared 64-bit up-counter (mtime), one comparator per
 * core (mtimecmp). Matching raises mip.MTIP -> mcause code 7.
 */

#define SIO_BASE            0xd0000000u
#define SIO_MTIME_CTRL      0x1a0
#define SIO_MTIME           0x1b0
#define SIO_MTIMEH          0x1b4
#define SIO_MTIMECMP        0x1b8
#define SIO_MTIMECMPH       0x1bc

#define MTIME_CTRL_EN        1u
#define MTIME_CTRL_FULLSPEED 2u

/* 150 MHz core clock: ~6.7 ms between preemptions. */
#define TIMER_INTERVAL      1000000u

static volatile uint32_t *sio_regs(void)
{
    return (volatile uint32_t *)SIO_BASE;
}

uint64_t platform_timer_now(void)
{
    volatile uint32_t *sio = sio_regs();
    uint32_t h0, l, h1;

    do {
        h0 = sio[SIO_MTIMEH / 4];
        l  = sio[SIO_MTIME  / 4];
        h1 = sio[SIO_MTIMEH / 4];
    } while (h0 != h1);

    return l | ((uint64_t)h1 << 32);
}

void platform_timer_arm(uint64_t deadline)
{
    volatile uint32_t *sio = sio_regs();

    /* Write low word first with a "far future" high word so the compare
     * never spuriously fires mid-update. */
    sio[SIO_MTIMECMP / 4]  = 0xffffffffu;
    sio[SIO_MTIMECMPH / 4] = (uint32_t)(deadline >> 32);
    sio[SIO_MTIMECMP / 4]  = (uint32_t)(deadline & 0xffffffffu);
}

bool platform_timer_is_irq(uintptr_t mcause)
{
    return mcause == PLATFORM_TIMER_MCAUSE;
}

/* Per-core part: arm this core's comparator and enable MTIE. MTIME is
 * shared, but MTIMECMP and MIE are per-core, so each core must call
 * this for itself. */
void timer_enable(void)
{
    platform_timer_arm(platform_timer_now() + TIMER_INTERVAL);

    uint32_t mie;
    __asm__ volatile ("csrr %0, 0x304" : "=r"(mie));
    mie |= 0x80u;
    __asm__ volatile ("csrw 0x304, %0" : : "r"(mie));
}

void timer_init(void)
{
    volatile uint32_t *sio = sio_regs();

    sio[SIO_MTIME_CTRL / 4] = MTIME_CTRL_EN | MTIME_CTRL_FULLSPEED;

    sio[SIO_MTIME / 4] = 0;
    sio[SIO_MTIMEH / 4] = 0;
    sio[SIO_MTIME / 4] = 0;

    timer_enable();
}

void timer_irq(void)
{
    platform_timer_arm(platform_timer_now() + TIMER_INTERVAL);

    if (current_process[current_core_id()] != NULL)
        yield();
}
