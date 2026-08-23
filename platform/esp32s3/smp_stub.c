#include <stdint.h>

#include "../../platform.h"

/*
 * The ESP32-S3 has two LX7 cores, but Scorpion's Xtensa port currently
 * schedules on core 0 only (current_core_id() reads PRID[3:0], which is
 * hardwired to 0 here). Multi-core scheduling needs per-core scheduler
 * contexts plus a boot handshake — future work; see docs/platforms.md.
 */

void smp_launch_core1(void (*entry)(void), uintptr_t sp)
{
    (void)entry;
    (void)sp;
}

void core1_main(void)
{
    /* unreachable */
}
