#include <string.h>

#include "alloc.h"
#include "console.h"
#include "driver.h"
#include "flash.h"
#include "fuse.h"
#include "loader.h"
#include "scorpion.h"

#define XIP_BASE            0x10000000u
#define CTRL_FLASH_OFFSET   0x00010000u
#define CTRL_XIP_ADDR       ((const uint8_t *)(XIP_BASE + CTRL_FLASH_OFFSET))

extern char __heap_start;
extern char __heap_end;

static void init_process(void *argument)
{
    (void)argument;

    log_info("init: booting, will spawn controller");

    /* packrom.py placed [4-byte size][controller.sexec] at flash offset
     * 0x10000.  The boot ROM loaded only the kernel to SRAM, so we read
     * the controller via XIP. */
    kernel_region.base = 0;
    kernel_region.size = (uintptr_t)&__heap_start;
    controller_region.base = 0;
    controller_region.size = 0;

    uint32_t ctrl_size;
    const uint8_t *ctrl_data;
    int pid;

    ctrl_size = *(const uint32_t *)CTRL_XIP_ADDR;
    ctrl_data = CTRL_XIP_ADDR + sizeof(ctrl_size);

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
    log_info("Scorpion kernel booting on RISC-V");

    alloc_init();
    log_info("allocator initialized");

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
