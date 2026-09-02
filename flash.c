#include <string.h>

#include "console.h"
#include "flash.h"
#include "platform.h"
#include "scorpion.h"

/*
 * Generic block-oriented flash layer shared by every platform.
 *
 * Platforms implement the byte/sector primitives declared in
 * platform.h; this file layers Scorpion's fixed-size blocks (see
 * flash.h) on top, including the read-modify-write dance NOR flash
 * needs when a block straddles already-programmed bytes.
 *
 * Block N lives at device offset FLASH_STORAGE_OFFSET + N*BLOCK_SIZE:
 * the storage region is reserved per platform so formatting FUSE can
 * never clobber the boot image or embedded payloads.
 *
 * Writes are buffered in a small write-back cache of whole 4 KB sector
 * images. An erase+program costs tens of ms with interrupts masked
 * (platform layer), so coalescing consecutive small writes to the same
 * sector into a single flush is worth more than any micro-speedup.
 * flash_sync() flushes everything; a dirty slot is also flushed lazily
 * when the cache fills.
 */

#define FLASH_SECTOR_SIZE 4096u
#define FLASH_SECTORS     (FLASH_TOTAL_SIZE / FLASH_SECTOR_SIZE)
#define FLASH_CACHE_SLOTS 4

typedef struct {
    bool dirty;
    bool valid;              /* data[] holds a live sector image */
    uint32_t sector;         /* sector index into the storage region */
    uint8_t data[FLASH_SECTOR_SIZE];
} FlashCacheSlot;

static FlashCacheSlot cache_slots[FLASH_CACHE_SLOTS];
static spinlock_t flash_cache_lock;

static uint32_t storage_offset(uint32_t block)
{
    return FLASH_STORAGE_OFFSET + block * FLASH_BLOCK_SIZE;
}

static uint32_t sector_index(uint32_t block)
{
    return block / (FLASH_SECTOR_SIZE / FLASH_BLOCK_SIZE);
}

static FlashCacheSlot *cache_lookup(uint32_t sector)
{
    for (unsigned i = 0; i < FLASH_CACHE_SLOTS; i++) {
        if (cache_slots[i].valid && cache_slots[i].sector == sector) {
            return &cache_slots[i];
        }
    }
    return NULL;
}

static int flush_slot(FlashCacheSlot *slot)
{
    uint32_t off = storage_offset(slot->sector * (FLASH_SECTOR_SIZE / FLASH_BLOCK_SIZE));
    int ret;

    if (!slot->dirty) {
        slot->valid = false;
        return 0;
    }

    ret = platform_flash_erase_sector(off);
    if (ret < 0) return ret;

    ret = platform_flash_program_bytes(off, slot->data, FLASH_SECTOR_SIZE);
    if (ret < 0) return ret;

    slot->dirty = false;
    slot->valid = false;
    return 0;
}

int flash_init(void)
{
    spinlock_init(&flash_cache_lock);
    return platform_flash_init();
}

int flash_sync(void)
{
    int ret = 0;

    spinlock_lock(&flash_cache_lock);

    for (unsigned i = 0; i < FLASH_CACHE_SLOTS; i++) {
        if (cache_slots[i].valid) {
            int r = flush_slot(&cache_slots[i]);
            if (r < 0 && ret == 0) ret = r;
        }
    }

    spinlock_unlock(&flash_cache_lock);
    return ret;
}

int flash_read(uint32_t block, void *buf, size_t len)
{
    uint32_t sector;
    FlashCacheSlot *slot;

    if (block >= FLASH_BLOCK_COUNT) return -1;
    if (len > FLASH_BLOCK_SIZE) return -1;
    if (buf == NULL) return -1;

    sector = sector_index(block);

    spinlock_lock(&flash_cache_lock);
    slot = cache_lookup(sector);
    if (slot != NULL && slot->dirty) {
        uint32_t off = (block * FLASH_BLOCK_SIZE) % FLASH_SECTOR_SIZE;
        memcpy(buf, slot->data + off, len);
        spinlock_unlock(&flash_cache_lock);
        return (int)len;
    }
    spinlock_unlock(&flash_cache_lock);

    return platform_flash_read_bytes(storage_offset(block), buf, len);
}

int flash_write(uint32_t block, const void *buf, size_t len)
{
    FlashCacheSlot *slot;
    uint32_t sector;
    uint32_t off;

    if (block >= FLASH_BLOCK_COUNT) return -1;
    if (len > FLASH_BLOCK_SIZE) return -1;
    if (buf == NULL) return -1;

    sector = sector_index(block);
    off = (block * FLASH_BLOCK_SIZE) % FLASH_SECTOR_SIZE;

    spinlock_lock(&flash_cache_lock);

    slot = cache_lookup(sector);
    if (slot == NULL) {
        /* Find a free slot; evict a dirty one if needed. */
        for (unsigned i = 0; i < FLASH_CACHE_SLOTS; i++) {
            if (!cache_slots[i].valid) {
                slot = &cache_slots[i];
                break;
            }
        }

        if (slot == NULL) {
            /* Evict the oldest dirty slot to make room. */
            for (unsigned i = 0; i < FLASH_CACHE_SLOTS; i++) {
                if (cache_slots[i].dirty) {
                    slot = &cache_slots[i];
                    break;
                }
            }
            if (slot == NULL) slot = &cache_slots[0];
        }

        /* A slot holding another sector must write it back first. */
        if (slot->dirty && flush_slot(slot) < 0) {
            spinlock_unlock(&flash_cache_lock);
            return -1;
        }

        slot->sector = sector;
        slot->valid = true;

        if (platform_flash_read_bytes(
                storage_offset(sector * (FLASH_SECTOR_SIZE / FLASH_BLOCK_SIZE)),
                slot->data, FLASH_SECTOR_SIZE) < 0) {
            slot->valid = false;
            spinlock_unlock(&flash_cache_lock);
            return -1;
        }
    }

    slot->dirty = true;
    memcpy(slot->data + off, buf, len);

    spinlock_unlock(&flash_cache_lock);
    return (int)len;
}

int flash_erase(uint32_t block)
{
    FlashCacheSlot *slot;
    uint32_t sector;

    if (block >= FLASH_BLOCK_COUNT) return -1;

    sector = sector_index(block);

    spinlock_lock(&flash_cache_lock);
    slot = cache_lookup(sector);
    if (slot != NULL) {
        slot->dirty = false;
        slot->valid = false;
    }
    spinlock_unlock(&flash_cache_lock);

    return platform_flash_erase_sector(storage_offset(block));
}