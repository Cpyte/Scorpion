#include "fuse.h"
#include "scorpion.h"

#define SYS_YIELD  0
#define SYS_EXIT   1
#define SYS_BLOCK  2
#define SYS_WAKE   3
#define SYS_SLEEP  4
#define SYS_SEND   5
#define SYS_RECV   6
#define SYS_OPEN   7
#define SYS_READ   8
#define SYS_WRITE  9
#define SYS_CLOSE  10
#define SYS_PUTC   11

extern void ecall_trigger(void);

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
    case 11:
        sysno = frame->a[7];

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

        case SYS_WAKE:
            wake_process((Process *)frame->a[0]);
            break;

        case SYS_PUTC:
            console_write((const char *)frame->a[0], frame->a[1]);
            break;

        case SYS_OPEN:
            frame->a[0] = (uintptr_t)fuse_open(
                (const char *)frame->a[0], frame->a[1]);
            break;

        case SYS_READ:
            frame->a[0] = (uintptr_t)fuse_read(
                (int)frame->a[0], (void *)frame->a[1], frame->a[2]);
            break;

        case SYS_WRITE:
            frame->a[0] = (uintptr_t)fuse_write(
                (int)frame->a[0], (const void *)frame->a[1], frame->a[2]);
            break;

        case SYS_CLOSE:
            frame->a[0] = (uintptr_t)fuse_close((int)frame->a[0]);
            break;

        case SYS_SLEEP:
            sleep_process(frame->a[0]);
            break;

        case SYS_SEND:
            frame->a[0] = (uintptr_t)send_message(
                process_by_pid(frame->a[0]),
                frame->a[1],
                (const void *)frame->a[2],
                frame->a[3]);
            break;

        case SYS_RECV:
            frame->a[0] = (uintptr_t)receive_message(
                (uint32_t *)frame->a[0],
                (void *)frame->a[1],
                frame->a[2],
                (uint16_t *)frame->a[3]);
            break;

        default:
            log_error("syscall: unhandled %s (%u) from pid=%u",
                      syscall_name(sysno), sysno,
                      current_process[core] ? current_process[core]->pid : 0);
            break;
        }
        break;

    default:
        panic("trap: unhandled mcause=%d mepc=%08x",
              frame->mcause, frame->mepc);
        break;
    }
}
