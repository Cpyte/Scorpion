#include "../../console.h"

/*
 * The Xtensa LX7 has no RISC-V PMP. Ring-1 user isolation would need
 * MMU/MPU region setup; the current port runs all processes in ring 0
 * and relies on the kernel's software privilege checks only. See
 * docs/platforms.md, "Security caveats".
 */

void pmp_init(void)
{
    log_warn("pmp: not implemented on this platform, user arena is unprotected");
}
