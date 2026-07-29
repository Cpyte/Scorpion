#include "alloc.h"
#include "console.h"
#include "driver.h"
#include "scorpion.h"

static Driver *driver_list;
static spinlock_t driver_lock;

static StageBuffer *stage_pool;
static StageBuffer *stage_free_list;
static spinlock_t stage_lock;

int register_driver(Driver *drv)
{
    if (drv == NULL) {
        return -1;
    }

    spinlock_lock(&driver_lock);
    drv->next = driver_list;
    driver_list = drv;
    spinlock_unlock(&driver_lock);

    log_info("driver: registered '%s'", drv->name);

    return 0;
}

Driver *find_driver(const char *name)
{
    Driver *d = driver_list;

    while (d != NULL) {
        unsigned i = 0;
        int match = 1;

        while (name[i] && d->name[i]) {
            if (name[i] != d->name[i]) {
                match = 0;
                break;
            }
            i++;
        }

        if (match && name[i] == '\0' && d->name[i] == '\0') {
            return d;
        }

        d = d->next;
    }

    return NULL;
}

int stage_pool_init(unsigned count, size_t buf_size)
{
    if (stage_pool != NULL) {
        return -1;
    }

    stage_pool = alloc_(count * sizeof(StageBuffer));

    if (stage_pool == NULL) {
        return -1;
    }

    for (unsigned i = 0; i < count; i++) {
        stage_pool[i].magic = STAGE_MAGIC;
        stage_pool[i].state = STAGE_FREE;
        stage_pool[i].data = alloc_(buf_size);
        stage_pool[i].capacity = buf_size;
        stage_pool[i].length = 0;
        stage_pool[i].next = stage_free_list;
        stage_free_list = &stage_pool[i];
    }

    log_info("stage: pool %u buffers x %u bytes", count, (unsigned)buf_size);

    return 0;
}

StageBuffer *stage_alloc(void)
{
    StageBuffer *buf;

    spinlock_lock(&stage_lock);
    buf = stage_free_list;

    if (buf != NULL) {
        stage_free_list = buf->next;
        buf->next = NULL;
        buf->state = STAGE_OWNED;
        buf->length = 0;
    }

    spinlock_unlock(&stage_lock);

    return buf;
}

void stage_free(StageBuffer *buf)
{
    if (buf == NULL || buf->magic != STAGE_MAGIC) {
        return;
    }

    spinlock_lock(&stage_lock);
    buf->state = STAGE_FREE;
    buf->length = 0;
    buf->next = stage_free_list;
    stage_free_list = buf;
    spinlock_unlock(&stage_lock);
}

int stage_submit(Driver *drv, StageBuffer *buf)
{
    if (drv == NULL || buf == NULL || buf->magic != STAGE_MAGIC) {
        return -1;
    }

    if (buf->state != STAGE_OWNED) {
        return -1;
    }

    buf->state = STAGE_BUSY;

    int ret = 0;

    if (drv->write != NULL) {
        ret = drv->write(drv, buf);
    }

    if (ret == 0) {
        buf->state = STAGE_DONE;
    } else {
        buf->state = STAGE_OWNED;
    }

    return ret;
}

StageBuffer *stage_reclaim(Driver *drv)
{
    (void)drv;

    return NULL;
}

int driver_init_all(void)
{
    int count = 0;
    Driver *d = driver_list;

    while (d != NULL) {
        if (d->init != NULL) {
            int ret = d->init(d);

            if (ret == 0) {
                log_info("driver: '%s' initialized", d->name);
                count++;
            } else {
                log_error("driver: '%s' init failed (%d)", d->name, ret);
            }
        }

        d = d->next;
    }

    return count;
}

void driver_print_all(void)
{
    Driver *d = driver_list;

    log_info("driver: --- registered drivers ---");

    while (d != NULL) {
        log_info("driver:   %s", d->name);
        d = d->next;
    }

    log_info("driver: ---");
}
