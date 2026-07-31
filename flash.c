#include <string.h>

#include "console.h"
#include "flash.h"
#include "scorpion.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#define XIP_BASE                    0x10000000u
#define FLASH_SECTOR_SIZE           4096u
#define ROM_FUNC_TABLE_ADDR         0x00000014u

#define BOOTROM_ENTRY_OFFSET          0x7dfc
#define BOOTROM_TABLE_LOOKUP_ENTRY    (BOOTROM_ENTRY_OFFSET - 2)
#define RT_FLAG_FUNC_RISCV            0x0001

#define ROM_CODE(c1, c2) ((c1) | ((c2) << 8))
#define ROM_FUNC_CONNECT_INTERNAL_FLASH   ROM_CODE('I', 'F')
#define ROM_FUNC_FLASH_EXIT_XIP           ROM_CODE('E', 'X')
#define ROM_FUNC_FLASH_RANGE_ERASE        ROM_CODE('R', 'E')
#define ROM_FUNC_FLASH_RANGE_PROGRAM      ROM_CODE('R', 'P')
#define ROM_FUNC_FLASH_FLUSH_CACHE        ROM_CODE('F', 'C')
#define ROM_FUNC_FLASH_ENTER_CMD_XIP      ROM_CODE('C', 'X')

typedef void *(*rom_table_lookup_fn)(uint32_t code, uint32_t flags);
typedef void (*rom_connect_internal_flash_fn)(void);
typedef void (*rom_flash_exit_xip_fn)(void);
typedef void (*rom_flash_range_erase_fn)(uint32_t addr, size_t count, uint32_t block_size, uint8_t timeout);
typedef void (*rom_flash_range_program_fn)(uint32_t addr, const uint8_t *data, size_t count);
typedef void (*rom_flash_flush_cache_fn)(void);
typedef void (*rom_flash_enter_cmd_xip_fn)(void);

static rom_connect_internal_flash_fn rom_connect_internal_flash;
static rom_flash_exit_xip_fn rom_flash_exit_xip;
static rom_flash_range_erase_fn rom_flash_range_erase;
static rom_flash_range_program_fn rom_flash_range_program;
static rom_flash_flush_cache_fn rom_flash_flush_cache;
static rom_flash_enter_cmd_xip_fn rom_flash_enter_cmd_xip;

static bool flash_ready;

/* Working buffer for the sector read-modify-write in flash_write().
 * Interrupts are masked around the erase/program, so no locking is
 * needed, but it must not live on the caller's stack (stacks are only
 * 4 KB here). */
static uint8_t flash_sector[FLASH_SECTOR_SIZE];

static void *rom_func_lookup(uint32_t code)
{
    rom_table_lookup_fn lookup = (rom_table_lookup_fn)(uintptr_t)(*(uint16_t *)(uintptr_t)BOOTROM_TABLE_LOOKUP_ENTRY);
    return lookup(code, RT_FLAG_FUNC_RISCV);
}

int flash_init(void)
{
    rom_connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase = (rom_flash_range_erase_fn)rom_func_lookup(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_range_program = (rom_flash_range_program_fn)rom_func_lookup(ROM_FUNC_FLASH_RANGE_PROGRAM);
    rom_flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup(ROM_FUNC_FLASH_FLUSH_CACHE);
    rom_flash_enter_cmd_xip = (rom_flash_enter_cmd_xip_fn)rom_func_lookup(ROM_FUNC_FLASH_ENTER_CMD_XIP);

    if (!rom_connect_internal_flash || !rom_flash_exit_xip ||
        !rom_flash_range_erase || !rom_flash_range_program ||
        !rom_flash_flush_cache || !rom_flash_enter_cmd_xip) {
        log_warn("flash: bootrom function lookup failed");
        return -1;
    }

    flash_ready = true;

    log_info("flash: %u bytes, %u blocks x %u",
             FLASH_TOTAL_SIZE, FLASH_BLOCK_COUNT, FLASH_BLOCK_SIZE);

    return 0;
}

int flash_read(uint32_t block, void *buf, size_t len)
{
    uint32_t offset;
    const uint8_t *flash_ptr;

    if (!flash_ready) return -1;
    if (block >= FLASH_BLOCK_COUNT) return -1;
    if (len > FLASH_BLOCK_SIZE) return -1;
    if (buf == NULL) return -1;

    offset = block * FLASH_BLOCK_SIZE;
    flash_ptr = (const uint8_t *)(XIP_BASE + offset);

    memcpy(buf, flash_ptr, len);

    return (int)len;
}

int flash_write(uint32_t block, const void *buf, size_t len)
{
    uint32_t offset;
    uint32_t sector_start;
    uint32_t sector_offset;
    uint32_t flags;

    if (!flash_ready) return -1;
    if (block >= FLASH_BLOCK_COUNT) return -1;
    if (len > FLASH_BLOCK_SIZE) return -1;
    if (buf == NULL) return -1;

    offset = block * FLASH_BLOCK_SIZE;
    sector_start = (offset / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
    sector_offset = offset - sector_start;

    /* Flash can only be erased a whole sector at a time, so read the full
     * sector, patch in this block, and reprogram the whole sector to keep
     * the sibling blocks intact. */
    memcpy(flash_sector, (const void *)(XIP_BASE + sector_start),
           FLASH_SECTOR_SIZE);
    memcpy(flash_sector + sector_offset, buf, len);

    /* While flash is out of XIP mode no code can be fetched or data read
     * from it, so mask interrupts for the whole erase/program sequence. */
    flags = irq_save();
    rom_connect_internal_flash();
    rom_flash_exit_xip();
    rom_flash_range_erase(sector_start, FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE, 0x78);
    rom_flash_range_program(sector_start, flash_sector, FLASH_SECTOR_SIZE);
    rom_flash_flush_cache();
    rom_flash_enter_cmd_xip();
    irq_restore(flags);

    return (int)len;
}

int flash_erase(uint32_t block)
{
    uint32_t offset;
    uint32_t sector_start;
    uint32_t flags;

    if (!flash_ready) return -1;
    if (block >= FLASH_BLOCK_COUNT) return -1;

    offset = block * FLASH_BLOCK_SIZE;
    sector_start = (offset / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    flags = irq_save();
    rom_connect_internal_flash();
    rom_flash_exit_xip();
    rom_flash_range_erase(sector_start, FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE, 0x78);
    rom_flash_flush_cache();
    rom_flash_enter_cmd_xip();
    irq_restore(flags);

    return 0;
}

#pragma GCC diagnostic pop
