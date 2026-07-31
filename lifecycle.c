#include <iso646.h>
#include <limits.h>

#include "alloc.h"
#include "scorpion.h"

ProcessQueue queue;
void *exclude_list[MAX_CORES];
CoreScheduler scheduler[MAX_CORES];
uint32_t tick_count = 0;
uint32_t next_pid = 1;

void process_exit(void)
{
    const unsigned core = current_core_id();
    Process *proc = current_process[core];

    proc->state = PROCESS_TERMINATED;
    current_process[core] = NULL;

    context_switch(&proc->context, &scheduler[core].context);
}

void add_process(Process *proc, uint16_t priority)
{
    ProcessNode *new_node = alloc_(sizeof(ProcessNode));

    new_node->process = proc;
    new_node->priority = priority;
    new_node->next = NULL;
    new_node->prev = NULL;

    ProcessNode *next = queue.head;

    if (next == NULL) {
        queue.head = new_node;
        queue.tail = new_node;
        return;
    }

    while (next != NULL && priority >= next->priority) {
        next = next->next;
    }

    if (next == NULL) {
        new_node->prev = queue.tail;
        queue.tail->next = new_node;
        queue.tail = new_node;
        return;
    }

    new_node->next = next;
    new_node->prev = next->prev;

    if (next->prev != NULL) {
        next->prev->next = new_node;
    } else {
        queue.head = new_node;
    }

    next->prev = new_node;
}

static void idle_process(void *argument)
{
    (void)argument;

    for (;;) {
        __asm__ volatile ("wfi");
        yield();
    }
}

void scheduler_init(void)
{
    const unsigned core = current_core_id();
    uintptr_t stack_top;

    stack_top = (uintptr_t)scheduler[core].stack + PROCESS_STACK_SIZE;
    stack_top &= ~(uintptr_t)0xF;

    scheduler[core].context.pc = (uintptr_t)scheduler_entry;
    scheduler[core].context.ra = (uintptr_t)scheduler_entry;
    scheduler[core].context.sp = stack_top;
    scheduler[core].context.gp = 0;
    scheduler[core].context.tp = 0;
    scheduler[core].context.mstatus = 0x1808u;

    for (unsigned i = 0; i < 12; i++) {
        scheduler[core].context.s[i] = 0;
    }

    Process *idle = alloc_(sizeof(Process));

    idle->pid = 0;
    idle->state = PROCESS_UNUSED;
    idle->privilege = PRIV_KERNEL;
    idle->entry = idle_process;
    idle->argument = NULL;
    idle->next = NULL;
    idle->prev = NULL;
    idle->context_initialized = false;

    add_process(idle, UINT16_MAX);
}

void scheduler_start(void)
{
    const unsigned core = current_core_id();

    context_switch(NULL, &scheduler[core].context);
}

void scheduler_entry(void)
{
    for (;;) {
        tick_count++;
        wake_sleeping_processes();
        killprocess();
        runprocess();
    }
}

int runprocess(void)
{
    const unsigned core = current_core_id();
    ProcessNode *p = queue.head;

    while (p != NULL) {
        Process *next = p->process;

        if (next->state == PROCESS_READY &&
            next != exclude_list[core]) {

            if (!next->context_initialized) {
                process_init_context(next);
            }

            next->state = PROCESS_RUNNING;
            current_process[core] = next;
            exclude_list[core] = NULL;

            context_switch(&scheduler[core].context, &next->context);

            return 0;
        }

        p = p->next;
    }

    p = queue.head;

    while (p != NULL) {
        Process *next = p->process;

        if (next->state == PROCESS_READY) {
            if (!next->context_initialized) {
                process_init_context(next);
            }

            next->state = PROCESS_RUNNING;
            current_process[core] = next;
            exclude_list[core] = NULL;

            context_switch(&scheduler[core].context, &next->context);

            return 0;
        }

        p = p->next;
    }

    return 1;
}

int killprocess(void)
{
    ProcessNode *p = queue.head;

    while (p != NULL) {
        if (p->process->state == PROCESS_TERMINATED) {
            ProcessNode *prev = p->prev;
            ProcessNode *next = p->next;

            if (prev != NULL)
                prev->next = next;
            else
                queue.head = next;

            if (next != NULL)
                next->prev = prev;
            else
                queue.tail = prev;

            if (p->process->alloc_base)
                ufree_((void *)p->process->alloc_base);
            if (p->process->stack_base) {
                ufree_(p->process->stack_base);
                free_(p->process->stack_base);
            }
            free_(p->process);
            free_(p);

            return 0;
        }

        p = p->next;
    }

    return 1;
}

int process_terminate(Process *proc)
{
    if (proc == NULL || proc->state == PROCESS_UNUSED) return -1;
    if (proc->privilege == PRIV_KERNEL) return -1;
    if (proc->state == PROCESS_RUNNING) return -1;

    proc->state = PROCESS_TERMINATED;

    if (proc->alloc_base)
        ufree_((void *)proc->alloc_base);
    if (proc->stack_base)
        ufree_(proc->stack_base);

    proc->alloc_base = 0;
    proc->text_base = 0;
    proc->text_size = 0;
    proc->data_base = 0;
    proc->data_size = 0;
    proc->bss_base = 0;
    proc->bss_size = 0;
    proc->stack_base = NULL;
    proc->stack_size = 0;

    return 0;
}

int evict_lowest_priority(size_t min_freed)
{
    ProcessNode *best = NULL;
    ProcessNode *p = queue.head;

    while (p != NULL) {
        Process *proc = p->process;
        if (proc->privilege == PRIV_USER &&
            proc->state != PROCESS_UNUSED &&
            proc->state != PROCESS_TERMINATED &&
            proc->state != PROCESS_RUNNING) {
            size_t proc_size = proc->text_size + proc->data_size +
                               proc->bss_size + proc->stack_size;
            if (proc_size >= min_freed) {
                if (best == NULL || p->priority > best->priority) {
                    best = p;
                }
            }
        }
        p = p->next;
    }

    if (best == NULL) return -1;

    ProcessNode *prev = best->prev;
    ProcessNode *next = best->next;
    if (prev != NULL)
        prev->next = next;
    else
        queue.head = next;
    if (next != NULL)
        next->prev = prev;
    else
        queue.tail = prev;

    Process *victim = best->process;
    log_info("evict: terminating pid=%u priority=%u",
             victim->pid, best->priority);
    process_terminate(victim);
    free_(victim);
    free_(best);
    return 0;
}

#define SIO_BASE            0xd0000000u
#define SIO_MTIME_CTRL      0x1a0
#define SIO_MTIME           0x1b0
#define SIO_MTIMEH          0x1b4
#define SIO_MTIMECMP        0x1b8
#define SIO_MTIMECMPH       0x1bc

#define MTIME_CTRL_EN       1u
#define MTIME_CTRL_FULLSPEED 2u

#define TIMER_INTERVAL      1000000u

static inline uint64_t timer_read(void)
{
    volatile uint32_t *sio = (volatile uint32_t *)SIO_BASE;
    uint32_t h0, l, h1;
    do {
        h0 = sio[SIO_MTIMEH / 4];
        l  = sio[SIO_MTIME  / 4];
        h1 = sio[SIO_MTIMEH / 4];
    } while (h0 != h1);
    return l | ((uint64_t)h1 << 32);
}

static inline void timer_set_cmp(uint64_t cmp)
{
    volatile uint32_t *sio = (volatile uint32_t *)SIO_BASE;
    sio[SIO_MTIMECMP / 4]  = 0xffffffffu;
    sio[SIO_MTIMECMPH / 4] = (uint32_t)(cmp >> 32);
    sio[SIO_MTIMECMP / 4]  = (uint32_t)(cmp & 0xffffffffu);
}

void timer_init(void)
{
    volatile uint32_t *sio = (volatile uint32_t *)SIO_BASE;

    sio[SIO_MTIME_CTRL / 4] = MTIME_CTRL_EN | MTIME_CTRL_FULLSPEED;

    sio[SIO_MTIME / 4] = 0;
    sio[SIO_MTIMEH / 4] = 0;
    sio[SIO_MTIME / 4] = 0;

    timer_set_cmp(TIMER_INTERVAL);

    uint32_t mie;
    __asm__ volatile ("csrr %0, 0x304" : "=r"(mie));
    mie |= 0x80u;
    __asm__ volatile ("csrw 0x304, %0" : : "r"(mie));
}

void timer_irq(void)
{
    uint64_t now = timer_read();
    timer_set_cmp(now + TIMER_INTERVAL);
    yield();
}

void yield(void)
{
    const unsigned core = current_core_id();
    Process *current = current_process[core];

    current->state = PROCESS_READY;
    exclude_list[core] = current;

    context_switch(&current->context, &scheduler[core].context);
}

void block_process(void)
{
    const unsigned core = current_core_id();
    Process *current = current_process[core];

    current->state = PROCESS_BLOCKED;

    context_switch(&current->context, &scheduler[core].context);
}

void wake_process(Process *p)
{
    if (p->state == PROCESS_BLOCKED) {
        p->state = PROCESS_READY;
        p->wake_tick = 0;
    }
}

Process *process_by_pid(uint32_t pid)
{
    ProcessNode *p = queue.head;

    while (p != NULL) {
        if (p->process->pid == pid) {
            return p->process;
        }
        p = p->next;
    }

    return NULL;
}

void sleep_process(uint32_t ticks)
{
    const unsigned core = current_core_id();
    Process *current = current_process[core];

    current->wake_tick = tick_count + ticks;
    current->state = PROCESS_BLOCKED;

    context_switch(&current->context, &scheduler[core].context);
}

void wake_sleeping_processes(void)
{
    ProcessNode *p = queue.head;

    while (p != NULL) {
        Process *proc = p->process;

        if (proc->state == PROCESS_BLOCKED && proc->wake_tick > 0 &&
            proc->wake_tick <= tick_count) {
            proc->state = PROCESS_READY;
            proc->wake_tick = 0;
        }

        p = p->next;
    }
}

int send_message(Process *target, uint32_t type, const void *data, size_t len)
{
    if (target == NULL) {
        return -1;
    }

    uint32_t flags = irq_save();

    if (target->msg_count >= IPC_QUEUE_DEPTH) {
        irq_restore(flags);
        return -1;
    }

    Message *msg = &target->msg_queue[target->msg_tail];

    msg->type = type;
    msg->sender_pid = current_process[current_core_id()]->pid;

    size_t copy_len = len < IPC_MAX_DATA ? len : IPC_MAX_DATA;

    for (size_t i = 0; i < copy_len; i++) {
        msg->data[i] = ((const uint8_t *)data)[i];
    }

    target->msg_tail = (target->msg_tail + 1) % IPC_QUEUE_DEPTH;
    target->msg_count++;

    if (target->state == PROCESS_BLOCKED) {
        target->state = PROCESS_READY;
        target->wake_tick = 0;
    }

    irq_restore(flags);
    return (int)copy_len;
}

int receive_message(uint32_t *type, void *buf, size_t len, uint16_t *sender_pid)
{
    const unsigned core = current_core_id();
    Process *current = current_process[core];

    for (;;) {
        uint32_t flags = irq_save();

        if (current->msg_count > 0) {
            Message *msg = &current->msg_queue[current->msg_head];

            *type = msg->type;
            *sender_pid = msg->sender_pid;

            size_t copy_len = len < IPC_MAX_DATA ? len : IPC_MAX_DATA;

            for (size_t i = 0; i < copy_len; i++) {
                ((uint8_t *)buf)[i] = msg->data[i];
            }

            current->msg_head = (current->msg_head + 1) % IPC_QUEUE_DEPTH;
            current->msg_count--;

            irq_restore(flags);
            return (int)copy_len;
        }

        irq_restore(flags);

        current->state = PROCESS_BLOCKED;
        context_switch(&current->context, &scheduler[core].context);
    }
}
