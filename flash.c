#include <string.h>

#include "console.h"
#include "flash.h"
#include "scorpion.h"

static uint8_t flash_memory[FLASH_TOTAL_SIZE];
static bool flash_ready;

int flash_init(void)
{
    flash_ready = true;

    memset(flash_memory, 0xFF, FLASH_TOTAL_SIZE);

    log_info("flash: %u bytes, %u blocks x %u",
             FLASH_TOTAL_SIZE, FLASH_BLOCK_COUNT, FLASH_BLOCK_SIZE);

    return 0;
}

int flash_read(uint32_t block, void *buf, size_t len)
{
    uint32_t offset;

    if (!flash_ready) return -1;
    if (block >= FLASH_BLOCK_COUNT) return -1;
    if (len > FLASH_BLOCK_SIZE) return -1;

    offset = block * FLASH_BLOCK_SIZE;

    for (size_t i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = flash_memory[offset + i];
    }

    return (int)len;
}

int flash_write(uint32_t block, const void *buf, size_t len)
{
    uint32_t offset;

    if (!flash_ready) return -1;
    if (block >= FLASH_BLOCK_COUNT) return -1;
    if (len > FLASH_BLOCK_SIZE) return -1;

    offset = block * FLASH_BLOCK_SIZE;

    for (size_t i = 0; i < len; i++) {
        flash_memory[offset + i] = ((const uint8_t *)buf)[i];
    }

    return (int)len;
}

int flash_erase(uint32_t block)
{
    uint32_t offset;

    if (!flash_ready) return -1;
    if (block >= FLASH_BLOCK_COUNT) return -1;

    offset = block * FLASH_BLOCK_SIZE;
    memset(&flash_memory[offset], 0xFF, FLASH_BLOCK_SIZE);

    return 0;
}
