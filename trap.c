#include "alloc.h"
#include "fuse.h"
#include "loader.h"
#include "platform.h"
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
#define SYS_LOADLIB   14

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

/* fuse_open() runs strlen() on the caller-supplied path while we are in
 * privileged mode, where memory protection does not apply — a user
 * pointer with no NUL inside the arena would make it scan into kernel
 * memory. Bound the scan: any valid FUSE name (max FUSE_NAME_LEN bytes
 * plus terminator) must have its NUL inside this window anyway. */
static bool user_path_ok(const Process *proc, const char *path)
{
    const uint8_t *p = (const uint8_t *)path;
    size_t i;

    if (!is_user_range(proc, p, 1)) return false;

    for (i = 0; i <= FUSE_NAME_LEN; i++) {
        if (&p[i] >= _user_arena_end) return false;
        if (p[i] == '\0') return true;
    }

    return false;
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
        [SYS_LOADLIB] = "LOADLIB",
    };

    if (n < sizeof(names) / sizeof(names[0]) && names[n] != NULL) {
        return names[n];
    }

    return "UNKNOWN";
}

void trap_handler(TrapFrame *frame)
{
    unsigned core = current_core_id();
    unsigned sysno;

    (void)core;

    switch (frame->mcause) {
#if defined(__xtensa__)
        /* Both aliases are EXCCAUSE_SYSCALL (1): a single case. */
    case MCAUSE_ECALL_U: {
#else
    case MCAUSE_ECALL_U:
    case MCAUSE_ECALL_M: {
#endif
        sysno = TRAP_SYSCALL_NO(frame);
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
            Process *p = process_by_pid(TRAP_ARG(frame, 0));
            if (p) wake_process(p);
            break;
        }

        case SYS_PUTC: {
            const void *buf = (const void *)TRAP_ARG(frame, 0);
            size_t len = TRAP_ARG(frame, 1);
            if (!is_user_range(cur, buf, len)) { TRAP_RET(frame, -1); break; }
            console_write((const char *)buf, len);
            break;
        }

        case SYS_OPEN: {
            const char *path = (const char *)TRAP_ARG(frame, 0);
            if (!user_path_ok(cur, path)) { TRAP_RET(frame, -1); break; }
            TRAP_RET(frame, fuse_open(path, TRAP_ARG(frame, 1)));
            break;
        }

        case SYS_READ: {
            void *buf = (void *)TRAP_ARG(frame, 1);
            size_t rlen = TRAP_ARG(frame, 2);
            if (!is_user_range(cur, buf, rlen)) { TRAP_RET(frame, -1); break; }
            TRAP_RET(frame, fuse_read((int)TRAP_ARG(frame, 0), buf, rlen));
            break;
        }

        case SYS_WRITE: {
            const void *buf = (const void *)TRAP_ARG(frame, 1);
            size_t wlen = TRAP_ARG(frame, 2);
            if (!is_user_range(cur, buf, wlen)) { TRAP_RET(frame, -1); break; }
            TRAP_RET(frame, fuse_write((int)TRAP_ARG(frame, 0), buf, wlen));
            break;
        }

        case SYS_CLOSE:
            TRAP_RET(frame, fuse_close((int)TRAP_ARG(frame, 0)));
            break;

        case SYS_SLEEP:
            sleep_process(TRAP_ARG(frame, 0));
            break;

        case SYS_SEND: {
            const void *msg = (const void *)TRAP_ARG(frame, 2);
            size_t msglen = TRAP_ARG(frame, 3);
            if (!is_user_range(cur, msg, msglen)) { TRAP_RET(frame, -1); break; }
            TRAP_RET(frame, send_message(
                process_by_pid(TRAP_ARG(frame, 0)),
                TRAP_ARG(frame, 1), msg, msglen));
            break;
        }

        case SYS_RECV: {
            uint32_t *type_out = (uint32_t *)TRAP_ARG(frame, 0);
            void *buf = (void *)TRAP_ARG(frame, 1);
            size_t rlen = TRAP_ARG(frame, 2);
            uint16_t *sender_out = (uint16_t *)TRAP_ARG(frame, 3);
            if (!is_user_range(cur, type_out, sizeof(*type_out)) ||
                !is_user_range(cur, buf, rlen) ||
                !is_user_range(cur, sender_out, sizeof(*sender_out))) {
                TRAP_RET(frame, -1);
                break;
            }
            TRAP_RET(frame, receive_message(
                type_out, buf, rlen, sender_out));
            break;
        }

        case SYS_SPAWN: {
            if (cur == NULL || cur->privilege > PRIV_CONTROLLER) {
                TRAP_RET(frame, -1);
                break;
            }
            const void *sef_data = (const void *)TRAP_ARG(frame, 0);
            size_t sef_size = TRAP_ARG(frame, 1);
            uint16_t priority = (uint16_t)TRAP_ARG(frame, 2);

            if (sef_data == NULL || !is_user_range(cur, sef_data, sef_size)) {
                TRAP_RET(frame, -1);
                break;
            }

            Process *new_proc = process_alloc();
            if (new_proc == NULL) {
                if (evict_lowest_priority(sizeof(Process)) != 0) {
                    TRAP_RET(frame, -2);
                    break;
                }
                new_proc = process_alloc();
                if (new_proc == NULL) {
                    TRAP_RET(frame, -2);
                    break;
                }
            }

            new_proc->pid = next_pid++;

            int ret = loader_load(sef_data, sef_size, new_proc);
            if (ret != 0) {
                size_t needed = new_proc->text_size + new_proc->data_size + new_proc->bss_size;
                if (needed == 0 || evict_lowest_priority(needed) != 0) {
                    free_(new_proc);
                    TRAP_RET(frame, -3);
                    break;
                }

                Process *retry = process_alloc();
                if (retry == NULL) {
                    free_(new_proc);
                    TRAP_RET(frame, -2);
                    break;
                }
                retry->pid = next_pid++;
                ret = loader_load(sef_data, sef_size, retry);
                if (ret != 0) {
                    free_(retry);
                    free_(new_proc);
                    TRAP_RET(frame, -3);
                    break;
                }
                new_proc = retry;
            }

            add_process(new_proc, priority);
            log_info("controller spawned pid=%u priority=%u",
                     new_proc->pid, priority);
            TRAP_RET(frame, new_proc->pid);
            break;
        }

        case SYS_TERMINATE: {
            if (cur == NULL || cur->privilege > PRIV_CONTROLLER) {
                TRAP_RET(frame, -1);
                break;
            }
            uint32_t pid = TRAP_ARG(frame, 0);
            Process *target = process_by_pid(pid);
            if (target == NULL) {
                TRAP_RET(frame, -2);
                break;
            }
            if (target->privilege <= PRIV_CONTROLLER && pid != cur->pid) {
                TRAP_RET(frame, -3);
                break;
            }
            int ret = process_terminate(target);
            TRAP_RET(frame, ret);
            break;
        }

        case SYS_LOADLIB: {
            if (cur == NULL || cur->privilege > PRIV_CONTROLLER) {
                TRAP_RET(frame, -1);
                break;
            }
            const void *data = (const void *)TRAP_ARG(frame, 0);
            size_t size = TRAP_ARG(frame, 1);
            if (size < 12 || !is_user_range(cur, data, size)) {
                TRAP_RET(frame, -2);
                break;
            }
            int lib_id = process_load_lib(cur, data, size);
            TRAP_RET(frame, lib_id);
            break;
        }

        default:
            log_error("syscall: unhandled %s (%u) from pid=%u",
                      syscall_name(sysno), sysno,
                      cur ? cur->pid : 0);
            break;
        }
        TRAP_ADVANCE_PC(frame);
        break;
        }

    default: {
        /* Platform timer interrupts are routed here by every backend —
         * ask the platform which synthesized cause belongs to it rather
         * than hard-coding an ISA-specific number. */
        if (platform_timer_is_irq(frame->mcause)) {
            timer_irq();
            break;
        }

        const unsigned core = current_core_id();
        Process *proc = current_process[core];
        if (proc != NULL && proc->privilege == PRIV_USER &&
            TRAP_IN_USER_MODE(frame)) {
            log_warn("user fault pid=%u cause=%u pc=%08x",
                     proc->pid, frame->mcause, frame->mepc);
            TRAP_ADVANCE_PC(frame);
            proc->state = PROCESS_TERMINATED;
            current_process[core] = NULL;
            context_switch(&proc->context, &scheduler[core].context);
        } else {
            panic("trap: unhandled cause=%d pc=%08x",
                  frame->mcause, frame->mepc);
        }
        break;
    }
    }
}

/* pmp_init() lives in platform/<name>/pmp*.c — RP2350 programs PMP TOR
 * entries; platforms without PMP provide a stub. */
