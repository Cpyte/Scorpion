#include <stdint.h>

/*
 * Xtensa "trap installation": on RISC-V this points mtvec at the trap
 * handler; here the equivalent is VECBASE. crt0.S already aims it at
 * _vector_table before main() runs — re-assert it so kernel_main()'s
 * log line reflects reality and early clobber attempts cannot hide.
 */

void trap_init(void)
{
    extern char _vector_table;

    uintptr_t vec = (uintptr_t)&_vector_table;
    __asm__ volatile ("wsr.vecbase %0; rsync" : : "r"(vec));
}
