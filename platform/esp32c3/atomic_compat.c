#include <stdbool.h>
#include <stdint.h>

#include "../../scorpion.h"

/*
 * The ESP32-C3 core has no A extension, so GCC cannot inline
 * atomic_exchange and this toolchain's libgcc does not ship the helper
 * either. Scorpion's only atomic user is spinlock_t; on the single-HB-
 * core C3 masking interrupts around the swap is a complete mutual
 * exclusion guarantee (the only preemptor is the trap handler).
 *
 * On RP2350 this file is not built: Hazard3 has amoswap and expands
 * the builtin inline.
 */

unsigned char __atomic_exchange_1(volatile void *ptr,
                                  unsigned char desired, int memorder)
{
    (void)memorder;

    uint32_t flags = irq_save();
    unsigned char old = *(volatile unsigned char *)ptr;
    *(volatile unsigned char *)ptr = desired;
    irq_restore(flags);

    return old;
}
