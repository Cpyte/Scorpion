#include "alloc.h"
#include "fuse.h"
#include "loader.h"
#include "sef.h"
#include "scorpion.h"

#define SYS_YIELD      0
#define SYS_EXIT       1
#define SYS_BLOCK      2
#define SYS_WAKE       3
#define SYS_SLEEP      4
#define SYS_SEND       5
#define SYS_RECV       6
#define SYS_OPEN       7
#define SYS_READ       8
#define SYS_WRITE      9
#define SYS_CLOSE     10
#define SYS_PUTC      11
#define SYS_SPAWN     12
#define SYS_TERMINATE 13

extern void ecall_trigger(void);

extern uint8_t _user_arena_start[];
extern uint8_t _user_arena_end[];

static bool is_user_range(const Process *proc, const void *ptr, size_t size)
{
    if (proc == NULL || proc->privilege != PRIV_USER) return true;
    const uint8_t *p = (const uint8_t *)ptr;
    return p >= _user_arena_start &&
           p + size >= p &&
           p + size <= (const uint8_t *)_user_arena_end;
}

static const char *syscall_name(unsigned n)
{
    static const char *names[] = {
        [SYS_YIELD] = "YIELD",
        [SYS_EXIT]  = "EXIT",
        [SYS_BLOCK] = "BLOCK",
        [SYS_WAKE]  = "WAKE",
        [SYS_SLEEP] = "SLEEP",
        [SYS_SEND]  = "SEND",
        [SYS_RECV]  = "RECV",
        [SYS_OPEN]  = "OPEN",
        [SYS_READ]  = "READ",
        [SYS_WRITE] = "WRITE",
        [SYS_CLOSE] = "CLOSE",
        [SYS_PUTC]  = "PUTC",
        [SYS_SPAWN] = "SPAWN",
        [SYS_TERMINATE] = "TERMINATE",
    };

    if (n < sizeof(names) / sizeof(names[0]) && names[n] != NULL) {
        return names[n];
    }

    return "UNKNOWN";
}

void trap_handler(RiscVTrapFrame *frame)
{
    unsigned core = current_core_id();
    unsigned sysno;

    (void)core;

    switch (frame->mcause) {
    case MCAUSE_TIMER_IRQ:
        timer_irq();
        break;

    case MCAUSE_ECALL_U:
    case MCAUSE_ECALL_M: {
        sysno = frame->a[7];
        Process *cur = current_process[core];

        switch (sysno) {
        case SYS_YIELD:
            yield();
            break;

        case SYS_EXIT:
            process_exit();
            break;

        case SYS_BLOCK:
            block_process();
            break;

        case SYS_WAKE: {
            Process *p = process_by_pid(frame->a[0]);
            if (p) wake_process(p);
            break;
        }

        case SYS_PUTC: {
            const void *buf = (const void *)frame->a[0];
            size_t len = frame->a[1];
            if (!is_user_range(cur, buf, len)) { frame->a[0] = -1; break; }
            console_write((const char *)buf, len);
            break;
        }

        case SYS_OPEN: {
            const char *path = (const char *)frame->a[0];
            if (!is_user_range(cur, path, 1)) { frame->a[0] = -1; break; }
            frame->a[0] = (uintptr_t)fuse_open(path, frame->a[1]);
            break;
        }

        case SYS_READ: {
            void *buf = (void *)frame->a[1];
            size_t rlen = frame->a[2];
            if (!is_user_range(cur, buf, rlen)) { frame->a[0] = -1; break; }
            frame->a[0] = (uintptr_t)fuse_read((int)frame->a[0], buf, rlen);
            break;
        }

        case SYS_WRITE: {
            const void *buf = (const void *)frame->a[1];
            size_t wlen = frame->a[2];
            if (!is_user_range(cur, buf, wlen)) { frame->a[0] = -1; break; }
            frame->a[0] = (uintptr_t)fuse_write((int)frame->a[0], buf, wlen);
            break;
        }

        case SYS_CLOSE:
            frame->a[0] = (uintptr_t)fuse_close((int)frame->a[0]);
            break;

        case SYS_SLEEP:
            sleep_process(frame->a[0]);
            break;

        case SYS_SEND: {
            const void *msg = (const void *)frame->a[2];
            size_t msglen = frame->a[3];
            if (!is_user_range(cur, msg, msglen)) { frame->a[0] = -1; break; }
            frame->a[0] = (uintptr_t)send_message(
                process_by_pid(frame->a[0]),
                frame->a[1], msg, msglen);
            break;
        }

        case SYS_RECV: {
            uint32_t *type_out = (uint32_t *)frame->a[0];
            void *buf = (void *)frame->a[1];
            size_t rlen = frame->a[2];
            uint16_t *sender_out = (uint16_t *)frame->a[3];
            if (!is_user_range(cur, type_out, sizeof(*type_out)) ||
                !is_user_range(cur, buf, rlen) ||
                !is_user_range(cur, sender_out, sizeof(*sender_out))) {
                frame->a[0] = (uintptr_t)-1;
                break;
            }
            frame->a[0] = (uintptr_t)receive_message(
                type_out, buf, rlen, sender_out);
            break;
        }

        case SYS_SPAWN: {
            if (cur == NULL || cur->privilege > PRIV_CONTROLLER) {
                frame->a[0] = (uintptr_t)-1;
                break;
            }
            const void *sef_data = (const void *)frame->a[0];
            size_t sef_size = frame->a[1];
            uint16_t priority = (uint16_t)frame->a[2];

            Process *new_proc = process_alloc();
            if (new_proc == NULL) {
                if (evict_lowest_priority(sizeof(Process)) == 0)
                    new_proc = process_alloc();
                if (new_proc == NULL) {
                    frame->a[0] = (uintptr_t)-2;
                    break;
                }
            }

            new_proc->pid = next_pid++;

            int ret = loader_load(sef_data, sef_size, new_proc);
            if (ret != 0) {
                size_t needed = new_proc->text_size + new_proc->data_size + new_proc->bss_size;
                free_(new_proc);
                if (needed > 0 && evict_lowest_priority(needed) == 0) {
                    new_proc = process_alloc();
                    if (new_proc == NULL) {
                        frame->a[0] = (uintptr_t)-2;
                        break;
                    }
                    new_proc->pid = next_pid++;
                    ret = loader_load(sef_data, sef_size, new_proc);
                }
            }

            if (ret != 0) {
                free_(new_proc);
                frame->a[0] = (uintptr_t)-3;
                break;
            }

            add_process(new_proc, priority);
            log_info("controller spawned pid=%u priority=%u",
                     new_proc->pid, priority);
            frame->a[0] = (uintptr_t)new_proc->pid;
            break;
        }

        case SYS_TERMINATE: {
            if (cur == NULL || cur->privilege > PRIV_CONTROLLER) {
                frame->a[0] = (uintptr_t)-1;
                break;
            }
            uint32_t pid = frame->a[0];
            Process *target = process_by_pid(pid);
            if (target == NULL) {
                frame->a[0] = (uintptr_t)-2;
                break;
            }
            if (target->privilege <= PRIV_CONTROLLER && pid != cur->pid) {
                frame->a[0] = (uintptr_t)-3;
                break;
            }
            int ret = process_terminate(target);
            frame->a[0] = (uintptr_t)ret;
            break;
        }

        default:
            log_error("syscall: unhandled %s (%u) from pid=%u",
                      syscall_name(sysno), sysno,
                      cur ? cur->pid : 0);
            break;
        }
        break;
        }

    default: {
        const unsigned core = current_core_id();
        Process *proc = current_process[core];
        if (proc != NULL && (frame->mstatus & 0x1800u) == 0) {
            log_warn("user fault pid=%u mcause=%u mepc=%08x",
                     proc->pid, frame->mcause, frame->mepc);
            frame->mepc += 4;
            proc->state = PROCESS_TERMINATED;
            current_process[core] = NULL;
            context_switch(&proc->context, &scheduler[core].context);
        } else {
            panic("trap: unhandled mcause=%d mepc=%08x",
                  frame->mcause, frame->mepc);
        }
        break;
    }
    }
}

void pmp_init(void)
{
    uintptr_t arena_start = (uintptr_t)_user_arena_start;
    uintptr_t arena_end   = (uintptr_t)_user_arena_end;

    if ((arena_start & 3) || (arena_end & 3))
        panic("pmp: user arena not 4-byte aligned (%08x–%08x)",
              (unsigned)arena_start, (unsigned)arena_end);

    uint32_t addr0 = (uint32_t)(arena_start >> 2);
    uint32_t addr1 = (uint32_t)(arena_end >> 2);
    uint32_t cfg0 = PMP_CFG_TOR;
    uint32_t cfg1 = PMP_CFG_TOR | PMP_CFG_RWX | PMP_CFG_L;

    __asm__ volatile ("csrw pmpaddr0, %0" : : "r"(addr0));
    __asm__ volatile ("csrw pmpaddr1, %0" : : "r"(addr1));
    __asm__ volatile ("csrw pmpcfg0, %0" : : "r"((cfg1 << 8) | cfg0));
}
