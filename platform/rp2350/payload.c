#include <stdint.h>

#include "platform.h"

/*
 * tools/packrom.py places [4-byte size][controller.sef] at flash offset
 * 0x10000. The boot ROM only loads the kernel into SRAM, so the payload
 * is read through the XIP window at 0x10000000.
 */

#define XIP_BASE          0x10000000u
#define CTRL_FLASH_OFFSET 0x00010000u

int platform_payload_controller(const uint8_t **data, uint32_t *size)
{
    const uint8_t *base = (const uint8_t *)(XIP_BASE + CTRL_FLASH_OFFSET);

    *size = *(const uint32_t *)base;
    *data = base + sizeof(uint32_t);

    if (*size == 0 || *size > (512u * 1024u)) {
        return -1;
    }

    return 0;
}
