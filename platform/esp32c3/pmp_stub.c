#include "../../console.h"
#include "../../scorpion.h"

/*
 * The ESP32-C3 RISC-V core implements machine mode but NOT the PMP
 * extension (it has a fixed PMA layout instead). U-mode protection via
 * PMP TOR entries is therefore unavailable: user processes still run at
 * MPP=0 and syscalls still work, but the kernel cannot hardware-trap
 * out-of-arena accesses. See docs/platforms.md, "Security caveats".
 */

void pmp_init(void)
{
    log_warn("pmp: not implemented on this platform, U-mode is unprotected");
}
