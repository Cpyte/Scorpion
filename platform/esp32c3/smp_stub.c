#include <stdint.h>

#include "../../platform.h"

/*
 * The ESP32-C3 has a single RV32IMC HP core (the LP core only exists on
 * C6), so SMP bring-up is a no-op. current_core_id() reads mhartid,
 * which is hardwired to 0 here.
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
