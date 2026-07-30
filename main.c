#include "alloc.h"
#include "console.h"
#include "driver.h"
#include "flash.h"
#include "fuse.h"
#include "loader.h"
#include "scorpion.h"

static void test_process(void *argument)
{
    unsigned count = 0;
    int fd;
    char buf[64];

    (void)argument;

    fd = fuse_open("/hello", FUSE_M_WRITE);

    if (fd >= 0) {
        const char *msg = "Scorpion FUSE\n";
        fuse_write(fd, msg, 14);
        fuse_close(fd);
    }

    for (;;) {
        log_info("test_process: iteration %u", count++);

        fd = fuse_open("/hello", FUSE_M_READ);

        if (fd >= 0) {
            int n = fuse_read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                log_info("test_process: file says '%s'", buf);
            }
            fuse_close(fd);
        }

        fuse_list();
        yield();
    }
}

static void kernel_main(void)
{
    Process *proc;

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

    proc = process_create(test_process, NULL);
    add_process(proc, 10);
    log_info("test process created");

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
