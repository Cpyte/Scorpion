#include <string.h>

#include "alloc.h"
#include "console.h"
#include "flash.h"
#include "fuse.h"
#include "scorpion.h"

#define FUSE_MAGIC 0x53434653

#define FUSE_BLOCKS_PER_FILE \
    ((FLASH_TOTAL_SIZE - FLASH_BLOCK_SIZE) / (FUSE_MAX_FILES * FLASH_BLOCK_SIZE))

typedef struct {
    uint32_t magic;
    uint32_t entry_count;
    uint32_t data_start;
    uint32_t reserved;
} FuseSuper;

typedef struct {
    char name[FUSE_NAME_LEN];
    uint32_t size;
    uint32_t first_block;
    uint32_t mode;
    uint8_t used;
    uint8_t reserved[3];
} FuseEntry;

typedef struct {
    bool used;
    int entry_idx;
    uint32_t offset;
} FuseFD;

static FuseFD fd_table[FUSE_MAX_FD];
static unsigned data_start_block;
static bool fuse_ready;

static int write_super(void)
{
    FuseSuper super;
    super.magic = FUSE_MAGIC;
    super.entry_count = 0;
    super.data_start = data_start_block;
    super.reserved = 0;

    for (unsigned i = 0; i < FUSE_MAX_FILES; i++) {
        fd_table[i].used = false;
    }

    return flash_write(0, &super, sizeof(super));
}

static int read_super(FuseSuper *super)
{
    return flash_read(0, super, sizeof(FuseSuper));
}

static int read_entry(unsigned idx, FuseEntry *entry)
{
    unsigned block = 1 + (idx * sizeof(FuseEntry)) / FLASH_BLOCK_SIZE;

    return flash_read(block, entry, sizeof(FuseEntry));
}

static int write_entry(unsigned idx, const FuseEntry *entry)
{
    unsigned block = 1 + (idx * sizeof(FuseEntry)) / FLASH_BLOCK_SIZE;
    unsigned offset = (idx * sizeof(FuseEntry)) % FLASH_BLOCK_SIZE;
    uint8_t buf[FLASH_BLOCK_SIZE];

    if (flash_read(block, buf, FLASH_BLOCK_SIZE) < 0) return -1;
    memcpy(buf + offset, entry, sizeof(FuseEntry));

    return flash_write(block, buf, FLASH_BLOCK_SIZE);
}

static int find_entry(const char *name)
{
    FuseEntry entry;

    for (unsigned i = 0; i < FUSE_MAX_FILES; i++) {
        if (read_entry(i, &entry) < 0) continue;
        if (!entry.used) continue;

        unsigned j = 0;
        int match = 1;

        while (j < FUSE_NAME_LEN) {
            if (entry.name[j] != name[j]) {
                match = 0;
                break;
            }
            if (name[j] == '\0') break;
            j++;
        }

        if (match) return (int)i;
    }

    return -1;
}

static int alloc_entry(void)
{
    FuseEntry entry;

    for (unsigned i = 0; i < FUSE_MAX_FILES; i++) {
        if (read_entry(i, &entry) < 0) continue;
        if (!entry.used) return (int)i;
    }

    return -1;
}

int fuse_init(void)
{
    FuseSuper super;

    for (unsigned i = 0; i < FUSE_MAX_FD; i++) {
        fd_table[i].used = false;
    }

    data_start_block = 1 + (FUSE_MAX_FILES * sizeof(FuseEntry) + FLASH_BLOCK_SIZE - 1) / FLASH_BLOCK_SIZE;

    if (read_super(&super) < 0 || super.magic != FUSE_MAGIC) {
        log_info("fuse: no filesystem, formatting");
        fuse_format();
    } else {
        log_info("fuse: filesystem ready, data at block %u", super.data_start);
    }

    fuse_ready = true;
    return 0;
}

int fuse_format(void)
{
    FuseEntry blank;

    data_start_block = 1 + (FUSE_MAX_FILES * sizeof(FuseEntry) + FLASH_BLOCK_SIZE - 1) / FLASH_BLOCK_SIZE;

    write_super();

    memset(&blank, 0, sizeof(blank));
    for (unsigned i = 0; i < FUSE_MAX_FILES; i++) {
        write_entry(i, &blank);
    }

    log_info("fuse: formatted (%u files, data at block %u)",
             FUSE_MAX_FILES, data_start_block);

    return 0;
}

int fuse_open(const char *name, unsigned mode)
{
    FuseEntry entry;
    int idx;

    if (!fuse_ready) return -1;
    if (name == NULL) return -1;

    idx = find_entry(name);

    if (idx < 0) {
        if (!(mode & FUSE_M_WRITE)) return -1;

        idx = alloc_entry();
        if (idx < 0) return -1;

        memset(&entry, 0, sizeof(entry));
        for (unsigned i = 0; i < FUSE_NAME_LEN && name[i]; i++) {
            entry.name[i] = name[i];
        }
        entry.size = 0;
        entry.first_block = data_start_block + (unsigned)idx * FUSE_BLOCKS_PER_FILE;
        entry.mode = mode;
        entry.used = 1;

        if (write_entry((unsigned)idx, &entry) < 0) return -1;

        {
            FuseSuper super;
            read_super(&super);
            super.entry_count++;
            flash_write(0, &super, sizeof(super));
        }
    } else {
        read_entry((unsigned)idx, &entry);
    }

    for (unsigned i = 0; i < FUSE_MAX_FD; i++) {
        if (!fd_table[i].used) {
            fd_table[i].used = true;
            fd_table[i].entry_idx = idx;
            fd_table[i].offset = 0;
            return (int)i;
        }
    }

    return -1;
}

int fuse_close(int fd)
{
    if (fd < 0 || fd >= (int)FUSE_MAX_FD) return -1;
    if (!fd_table[fd].used) return -1;

    fd_table[fd].used = false;

    return 0;
}

int fuse_read(int fd, void *buf, size_t size)
{
    FuseEntry entry;
    uint32_t remaining;
    size_t total;
    uint32_t file_pos;
    uint8_t block_buf[FLASH_BLOCK_SIZE];

    if (fd < 0 || fd >= (int)FUSE_MAX_FD) return -1;
    if (!fd_table[fd].used) return -1;

    read_entry((unsigned)fd_table[fd].entry_idx, &entry);

    if (fd_table[fd].offset >= entry.size) return 0;

    remaining = entry.size - fd_table[fd].offset;
    if (size > remaining) size = remaining;

    file_pos = fd_table[fd].offset;
    total = 0;

    while (size > 0) {
        unsigned data_block = (file_pos / FLASH_BLOCK_SIZE);
        uint32_t block_addr = entry.first_block + data_block;
        unsigned block_off = file_pos % FLASH_BLOCK_SIZE;
        size_t to_copy = FLASH_BLOCK_SIZE - block_off;

        if (to_copy > size) to_copy = size;

        if (flash_read(block_addr, block_buf, FLASH_BLOCK_SIZE) < 0) return -1;
        memcpy((uint8_t *)buf + total, block_buf + block_off, to_copy);

        file_pos += (uint32_t)to_copy;
        total += to_copy;
        size -= to_copy;
    }

    fd_table[fd].offset = file_pos;

    return (int)total;
}

int fuse_write(int fd, const void *buf, size_t size)
{
    FuseEntry entry;
    size_t total;
    uint32_t file_pos;
    uint8_t block_buf[FLASH_BLOCK_SIZE];

    if (fd < 0 || fd >= (int)FUSE_MAX_FD) return -1;
    if (!fd_table[fd].used) return -1;

    read_entry((unsigned)fd_table[fd].entry_idx, &entry);

    file_pos = fd_table[fd].offset;
    total = 0;

    while (size > 0) {
        unsigned data_block = (file_pos / FLASH_BLOCK_SIZE);
        uint32_t block_addr = entry.first_block + data_block;
        unsigned block_off = file_pos % FLASH_BLOCK_SIZE;
        size_t to_copy = FLASH_BLOCK_SIZE - block_off;

        if (to_copy > size) to_copy = size;

        if (flash_read(block_addr, block_buf, FLASH_BLOCK_SIZE) < 0) return -1;
        memcpy(block_buf + block_off, (const uint8_t *)buf + total, to_copy);
        if (flash_write(block_addr, block_buf, FLASH_BLOCK_SIZE) < 0) return -1;

        file_pos += (uint32_t)to_copy;
        total += to_copy;
        size -= to_copy;
    }

    fd_table[fd].offset = file_pos;

    if (file_pos > entry.size) {
        entry.size = file_pos;
        write_entry((unsigned)fd_table[fd].entry_idx, &entry);
    }

    return (int)total;
}

int fuse_list(void)
{
    FuseEntry entry;

    log_info("fuse: --- files ---");

    for (unsigned i = 0; i < FUSE_MAX_FILES; i++) {
        if (read_entry(i, &entry) < 0) continue;

        if (entry.used) {
            log_info("fuse:   %s  (%u bytes)", entry.name, (unsigned)entry.size);
        }
    }

    log_info("fuse: ---");

    return 0;
}
