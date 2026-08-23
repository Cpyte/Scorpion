#include <stdint.h>

#include "scorpion.h"
#include "platform.h"

extern char _user_arena_start;
extern char _user_arena_end;

/* PMP RISC-V CSRs live only on this platform, so their encodings are
 * defined here rather than in the shared scorpion.h. */

#define PMP_CFG_OFF   0x00u
#define PMP_CFG_TOR   0x18u
#define PMP_CFG_NAPOT 0x1Cu
#define PMP_CFG_R     0x04u
#define PMP_CFG_W     0x02u
#define PMP_CFG_X     0x01u
#define PMP_CFG_L     0x80u
#define PMP_CFG_RWX   (PMP_CFG_R | PMP_CFG_W | PMP_CFG_X)

/*
 * RP2350 user-mode protection: two PMP TOR entries fence the user
 * arena — entry 0 denies everything below the arena, entry 1 grants
 * RWX inside it with L=1 so U-mode is bound by both rules while M-mode
 * ignores them.
 */

void pmp_init(void)
{
    uintptr_t arena_start = (uintptr_t)&_user_arena_start;
    uintptr_t arena_end   = (uintptr_t)&_user_arena_end;

    if ((arena_start & 3) || (arena_end & 3))
        panic("pmp: user arena not 4-byte aligned (%08x-%08x)",
              (unsigned)arena_start, (unsigned)arena_end);

    uint32_t addr0 = (uint32_t)(arena_start >> 2);
    uint32_t addr1 = (uint32_t)(arena_end >> 2);
    uint32_t cfg0 = PMP_CFG_TOR;
    uint32_t cfg1 = PMP_CFG_TOR | PMP_CFG_RWX | PMP_CFG_L;

    __asm__ volatile ("csrw pmpaddr0, %0" : : "r"(addr0));
    __asm__ volatile ("csrw pmpaddr1, %0" : : "r"(addr1));
    __asm__ volatile ("csrw pmpcfg0, %0" : : "r"((cfg1 << 8) | cfg0));
}

