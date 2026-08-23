#include <string.h>

#include "console.h"
#include "flash.h"
#include "platform.h"

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
 */

#define FLASH_SECTOR_SIZE 4096u

static uint32_t storage_offset(uint32_t block)
{
    return FLASH_STORAGE_OFFSET + block * FLASH_BLOCK_SIZE;
}

int flash_init(void)
{
    return platform_flash_init();
}

int flash_read(uint32_t block, void *buf, size_t len)
{
    if (block >= FLASH_BLOCK_COUNT) return -1;
    if (len > FLASH_BLOCK_SIZE) return -1;
    if (buf == NULL) return -1;

    return platform_flash_read_bytes(storage_offset(block), buf, len);
}

/* Working buffer for the sector read-modify-write in flash_write().
 * Interrupts are masked around erase/program inside the platform layer,
 * so no extra locking is needed here — but the buffer must not live on
 * a 4 KB process stack. */
static uint8_t rmw_sector[FLASH_SECTOR_SIZE];

int flash_write(uint32_t block, const void *buf, size_t len)
{
    uint32_t offset;
    uint32_t sector_start;
    uint32_t sector_offset;
    int ret;

    if (block >= FLASH_BLOCK_COUNT) return -1;
    if (len > FLASH_BLOCK_SIZE) return -1;
    if (buf == NULL) return -1;

    offset = storage_offset(block);
    sector_start = offset & ~(uint32_t)(FLASH_SECTOR_SIZE - 1u);
    sector_offset = offset - sector_start;

    /* Flash can only be erased a whole sector at a time, so read the
     * full sector, patch in this block, and reprogram the whole sector
     * to keep sibling blocks intact. */
    ret = platform_flash_read_bytes(sector_start, rmw_sector, FLASH_SECTOR_SIZE);
    if (ret < 0) return ret;

    memcpy(rmw_sector + sector_offset, buf, len);

    ret = platform_flash_erase_sector(sector_start);
    if (ret < 0) return ret;

    return platform_flash_program_bytes(sector_start, rmw_sector, FLASH_SECTOR_SIZE);
}

int flash_erase(uint32_t block)
{
    uint32_t offset;

    if (block >= FLASH_BLOCK_COUNT) return -1;

    offset = storage_offset(block);

    return platform_flash_erase_sector(offset);
}
