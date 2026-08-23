#include <string.h>

#include "alloc.h"
#include "console.h"
#include "driver.h"
#include "flash.h"
#include "fuse.h"
#include "loader.h"
#include "platform.h"
#include "scorpion.h"

extern char __heap_start;
extern char __heap_end;

static void init_process(void *argument)
{
    (void)argument;

    log_info("init: booting, will spawn controller");

    /* packrom.py placed [4-byte size][controller.sef] next to the
     * kernel image; platform_payload_controller() knows where. */
    kernel_region.base = 0;
    kernel_region.size = (uintptr_t)&__heap_start;
    controller_region.base = 0;
    controller_region.size = 0;

    const uint8_t *ctrl_data = NULL;
    uint32_t ctrl_size = 0;
    int pid = -1;

    if (platform_payload_controller(&ctrl_data, &ctrl_size) == 0) {
#if defined(__xtensa__)
        /* call0 syscall convention: sysno in a2, args a3–a6, result
         * back in a2. */
        __asm__ volatile (
            "mov a3, %[data]\n"
            "mov a4, %[size]\n"
            "movi a5, 1\n"
            "movi a2, 12\n"
            "syscall\n"
            "mov %[pid], a2\n"
            : [pid] "=r" (pid)
            : [data] "r" (ctrl_data),
              [size] "r" (ctrl_size)
            : "a2", "a3", "a4", "a5", "memory"
        );
#else
        __asm__ volatile (
            "mv a0, %[data]\n"
            "mv a1, %[size]\n"
            "li a2, 1\n"
            "li a7, 12\n"
            "ecall\n"
            "mv %[pid], a0\n"
            : [pid] "=r" (pid)
            : [data] "r" (ctrl_data),
              [size] "r" (ctrl_size)
            : "a0", "a1", "a2", "a7", "memory"
        );
#endif
    } else {
        log_warn("init: no embedded controller payload on this platform");
    }

    if (pid < 0) {
        log_error("init: failed to spawn controller (%d)", pid);
    } else {
        log_info("init: spawned controller pid=%d", pid);
    }

    for (;;) {
        yield();
    }
}

static void kernel_main(void)
{
    console_init();
    log_info("Scorpion kernel booting (%s)", PLATFORM_NAME);

    alloc_init();
    log_info("allocator initialized");

    user_arena_init();
    log_info("user arena initialized");

    pmp_init();
    log_info("PMP configured (U-mode protection enabled)");

    trap_init();
    log_info("trap handler installed");

    flash_init();
    log_info("micro flash ready");

    fuse_init();
    log_info("FUSE filesystem ready");

    loader_init();
    log_info("executable loader ready");

    stage_pool_init(8, 512);
    log_info("stage pool ready");

    timer_init();
    log_info("timer initialized");

    Process *init = process_create(init_process, NULL);
    init->privilege = PRIV_CONTROLLER;
    add_process(init, 1);
    log_info("init process created (pid=%u)", init->pid);

    scheduler_init();
    log_info("scheduler initialized");

    smp_launch_core1(core1_main,
                     ((uintptr_t)scheduler[1].stack + PROCESS_STACK_SIZE)
                     & ~(uintptr_t)0xF);
    log_info("secondary core started");

    driver_print_all();

    log_info("starting scheduler");
    scheduler_start();

    for (;;) {}
}

int main(void)
{
    kernel_main();
    return 0;
}
