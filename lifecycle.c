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

    for (unsigned i = 0; i < 12; i++) {
        scheduler[core].context.s[i] = 0;
    }

    Process *idle = alloc_(sizeof(Process));

    idle->pid = 0;
    idle->state = PROCESS_UNUSED;
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

            free_(p->process->stack_base);
            free_(p->process);
            free_(p);

            return 0;
        }

        p = p->next;
    }

    return 1;
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

    if (target->msg_count >= IPC_QUEUE_DEPTH) {
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

    return (int)copy_len;
}

int receive_message(uint32_t *type, void *buf, size_t len, uint16_t *sender_pid)
{
    const unsigned core = current_core_id();
    Process *current = current_process[core];

    for (;;) {
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

            return (int)copy_len;
        }

        current->state = PROCESS_BLOCKED;
        context_switch(&current->context, &scheduler[core].context);
    }
}
