#ifndef SCORPION_FLASH_H
#define SCORPION_FLASH_H

#include <stddef.h>
#include <stdint.h>

#define FLASH_BLOCK_SIZE  256u
#define FLASH_BLOCK_COUNT 256u
#define FLASH_TOTAL_SIZE  (FLASH_BLOCK_SIZE * FLASH_BLOCK_COUNT)

int flash_init(void);
int flash_read(uint32_t block, void *buf, size_t len);
int flash_write(uint32_t block, const void *buf, size_t len);
int flash_erase(uint32_t block);
int flash_sync(void);

#endif
