#include "console.h"
#include "scorpion.h"

#define SIO_BASE       0xd0000000u
#define SIO_FIFO_ST    0x50u
#define SIO_FIFO_WR    0x54u
#define SIO_FIFO_RD    0x58u

#define SIO_FIFO_ST_RDY (1u << 1)  /* FIFO can accept a write */
#define SIO_FIFO_ST_VLD (1u << 0)  /* FIFO has a word to read */

static inline void fifo_push(uint32_t word)
{
    volatile uint32_t *sio = (volatile uint32_t *)SIO_BASE;

    while (!(sio[SIO_FIFO_ST / 4] & SIO_FIFO_ST_RDY)) { }
    sio[SIO_FIFO_WR / 4] = word;
}

static inline uint32_t fifo_pop(void)
{
    volatile uint32_t *sio = (volatile uint32_t *)SIO_BASE;

    while (!(sio[SIO_FIFO_ST / 4] & SIO_FIFO_ST_VLD)) { }
    return sio[SIO_FIFO_RD / 4];
}

/* Start core 1.  The bootrom parks every non-core-0 Hazard3 core in a loop
 * that echoes words from the SIO FIFO; pushing {0, 0, 1, mtvec, sp, entry}
 * makes it install mtvec/sp and jump to entry.  Each word must be echoed
 * back before the next is sent, restarting the sequence on a mismatch.
 * (Same protocol as pico-sdk multicore_launch_core1_raw.) */
void smp_launch_core1(void (*entry)(void), uintptr_t sp)
{
    uint32_t mtvec;
    uint32_t seq[6];
    uint32_t flags;
    unsigned i;

    __asm__ volatile ("csrr %0, mtvec" : "=r"(mtvec));

    seq[0] = 0;
    seq[1] = 0;
    seq[2] = 1;
    seq[3] = mtvec;
    seq[4] = (uint32_t)sp;
    seq[5] = (uint32_t)(uintptr_t)entry;

    flags = irq_save();
    i = 0;
    while (i < 6) {
        fifo_push(seq[i]);
        i = (fifo_pop() == seq[i]) ? i + 1 : 0;
    }
    irq_restore(flags);

    log_info("smp: core 1 launched (entry=%p)", (void *)entry);
}

/* Core 1's entry.  It inherits core 0's mtvec (passed in the handshake), so
 * it only needs its own timer interrupt and scheduler context.  Must not
 * touch anything else -- console, alloc, flash, fuse are core-0 only. */
void core1_main(void)
{
    timer_enable();
    scheduler_start_core();
}
