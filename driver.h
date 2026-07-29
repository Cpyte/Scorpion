#ifndef SCORPION_DRIVER_H
#define SCORPION_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#define STAGE_MAGIC 0x53435447

typedef enum {
    STAGE_FREE,
    STAGE_OWNED,
    STAGE_BUSY,
    STAGE_DONE,
} StageState;

typedef struct StageBuffer {
    uint32_t magic;
    StageState state;
    uint8_t *data;
    size_t capacity;
    size_t length;
    struct StageBuffer *next;
} StageBuffer;

typedef struct Driver {
    const char *name;
    int (*init)(struct Driver *self);
    int (*open)(struct Driver *self);
    int (*close)(struct Driver *self);
    int (*write)(struct Driver *self, StageBuffer *buf);
    int (*read)(struct Driver *self, StageBuffer *buf);
    int (*ioctl)(struct Driver *self, unsigned req, void *arg);
    void *priv;
    struct Driver *next;
} Driver;

int register_driver(Driver *drv);
Driver *find_driver(const char *name);

int stage_pool_init(unsigned count, size_t buf_size);
StageBuffer *stage_alloc(void);
void stage_free(StageBuffer *buf);
int stage_submit(Driver *drv, StageBuffer *buf);
StageBuffer *stage_reclaim(Driver *drv);

int driver_init_all(void);
void driver_print_all(void);

#endif
