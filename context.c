#include "alloc.h"
#include "scorpion.h"

Process *current_process[MAX_CORES];

unsigned current_core_id(void)
{
#if defined(__xtensa__)
    /* PRID[3:0] carries the core number on single- and multi-core
     * Xtensa parts alike. */
    uint32_t prid;
    __asm__ volatile ("rsr.prid %0" : "=r"(prid));
    return (unsigned)(prid & 0xFu);
#else
    uintptr_t hart_id;

    __asm__ volatile (
        "csrr %0, mhartid"
        : "=r"(hart_id)
    );

    return (unsigned)hart_id;
#endif
}

static void process_trampoline(void)
{
    const unsigned core = current_core_id();
    Process *proc = current_process[core];

    proc->state = PROCESS_RUNNING;

    proc->entry(proc->argument);

    process_exit();

    for (;;) {
        ARCH_IDLE();
    }
}

static void init_context_regs(Process *process, uintptr_t pc, uintptr_t sp,
                               uint32_t mstatus)
{
#if defined(__xtensa__)
    (void)mstatus;
#endif
    process->context.pc = pc;
    process->context.sp = sp;
#if defined(__xtensa__)
    /* call0: nothing else is live across a switch. */
    for (unsigned i = 0; i < 4; i++)
        process->context.s[i] = 0;
#else
    process->context.ra = 0;
    process->context.gp = 0;
    process->context.tp = 0;
    for (unsigned i = 0; i < 12; i++)
        process->context.s[i] = 0;
    process->context.mstatus = mstatus;
#endif
    process->state = PROCESS_READY;
    process->context_initialized = true;
}

void process_init_context(Process *process)
{
    process->stack_size = PROCESS_STACK_SIZE;
    process->stack_base = alloc_(process->stack_size);

    uintptr_t stack_top =
        (uintptr_t)process->stack_base +
        process->stack_size;

    stack_top &= ~(uintptr_t)0xF;

    init_context_regs(process, (uintptr_t)process_trampoline,
                      stack_top, 0x1808u);
}

void process_init_user_context(Process *process, uintptr_t pc, uintptr_t sp)
{
    process->stack_size = PROCESS_STACK_SIZE;
    process->stack_base = ualloc_(process->stack_size);

    uintptr_t user_stack_top =
        (uintptr_t)process->stack_base + process->stack_size;
    user_stack_top &= ~(uintptr_t)0xF;

    if (sp == 0) sp = user_stack_top;

    init_context_regs(process, pc, sp, 0x0088u);
}

