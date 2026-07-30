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

    for (;;) {
        __asm__ volatile ("wfi");
    }
}

static void init_context_regs(Process *process, uintptr_t pc, uintptr_t sp,
                               uint32_t mstatus)
{
    process->context.pc = pc;
    process->context.ra = 0;
    process->context.sp = sp;
    process->context.gp = 0;
    process->context.tp = 0;
    for (unsigned i = 0; i < 12; i++)
        process->context.s[i] = 0;
    process->context.mstatus = mstatus;
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

