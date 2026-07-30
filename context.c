#include "alloc.h"
#include "scorpion.h"

Process *current_process[MAX_CORES];

unsigned current_core_id(void)
{
    uintptr_t hart_id;

    __asm__ volatile (
        "csrr %0, mhartid"
        : "=r"(hart_id)
    );

    return (unsigned)hart_id;
}

static void process_trampoline(void)
{
    const unsigned core = current_core_id();
    Process *proc = current_process[core];

    proc->state = PROCESS_RUNNING;

    proc->entry(proc->argument);

    process_exit();

    /*
     * process_exit() should not return.
     * If it does, the process has nowhere valid to continue.
     */
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

void process_init_context(Process *process)
{
    process->stack_size = PROCESS_STACK_SIZE;
    process->stack_base = alloc_(process->stack_size);

    uintptr_t stack_top =
        (uintptr_t)process->stack_base +
        process->stack_size;

    stack_top &= ~(uintptr_t)0xF;

    process->context.pc = (uintptr_t)process_trampoline;
    process->context.ra = 0;
    process->context.sp = stack_top;

    process->context.gp = 0;
    process->context.tp = 0;

    for (unsigned i = 0; i < 12; i++) {
        process->context.s[i] = 0;
    }

    process->context.mstatus = 0x1808u;

    process->state = PROCESS_READY;
    process->context_initialized = true;
}

