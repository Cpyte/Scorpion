#include <string.h>

#include <stdint.h>

#include "../../console.h"
#include "../../flash.h"
#include "../../platform.h"
#include "../../scorpion.h"

/*
 * Flash access for the ESP32-C3 via the legacy SPI-flash driver in ROM.
 *
 * Addresses below are fixed by the C3 mask ROM and match Espressif's
 * generated linker fragment (components/esp_rom/esp32c3/ld/
 * esp32c3.rom.ld, group "spiflash_legacy"):
 *
 *   esp_rom_spiflash_erase_sector = 0x40000128
 *   esp_rom_spiflash_write        = 0x4000012c
 *   esp_rom_spiflash_read         = 0x40000130
 *
 * The boot ROM attached and configured the flash chip before jumping to
 * our image, so the driver's global chip state is already valid. All
 * three functions execute from IRAM (ROM), so they are safe to call
 * from a kernel that runs entirely from RAM; interrupts are masked
 * around each operation anyway because erase/program stall the CPU for
 * tens of milliseconds.
 *
 * Alignment rules of the legacy ROM API:
 *   - read:  address must be word-aligned; length is rounded up here
 *            and served from a bounce buffer.
 *   - write: address AND length must be multiples of 4. The generic
 *            block layer always programs whole 4 KB-aligned sectors,
 *            which satisfies this.
 *   - erase: takes a SECTOR NUMBER (offset / 4096), not an address.
 */

#define ROM_SPIFLASH_ERASE_SECTOR 0x40000128u
#define ROM_SPIFLASH_WRITE        0x4000012cu
#define ROM_SPIFLASH_READ         0x40000130u

typedef int (*rom_erase_sector_fn)(uint32_t sector_number);
typedef int (*rom_write_fn)(uint32_t addr, const uint32_t *data, uint32_t len);
typedef int (*rom_read_fn)(uint32_t addr, uint32_t *data, uint32_t len);

#define FLASH_SECTOR_SIZE 4096u

static bool flash_ready;

/* Bounce buffer for unaligned/unbounded reads (must survive on the
 * heap-free boot path, hence static). */
static uint32_t read_bounce[FLASH_BLOCK_SIZE / sizeof(uint32_t)];

int platform_flash_init(void)
{
    /* Sanity-check that the ROM function pointers at least point into
     * the ROM address window. */
    if ((ROM_SPIFLASH_READ & 0xff000000u) != 0x40000000u) {
        log_warn("flash: ROM spiflash entry points look wrong");
        return -1;
    }

    flash_ready = true;

    log_info("flash: %u bytes, %u blocks x %u (via ROM spiflash)",
             FLASH_TOTAL_SIZE, FLASH_BLOCK_COUNT, FLASH_BLOCK_SIZE);

    return 0;
}

int platform_flash_read_bytes(uint32_t off, void *buf, size_t len)
{
    static rom_read_fn rom_read =
        (rom_read_fn)(uintptr_t)ROM_SPIFLASH_READ;

    uint32_t aligned_off;
    size_t padded_len;

    if (!flash_ready || buf == NULL || len == 0 ||
        (len > sizeof(read_bounce))) {
        return -1;
    }

    /* Word-align the start and round the length up, then copy out the
     * requested slice from the bounce buffer. */
    aligned_off = off & ~3u;
    padded_len = ALIGN_UP(len + (off - aligned_off), sizeof(uint32_t));

    if (padded_len > sizeof(read_bounce)) {
        return -1;
    }

    uint32_t flags = irq_save();
    int ret = rom_read(aligned_off, read_bounce, (uint32_t)padded_len);
    irq_restore(flags);

    if (ret != 0) {
        return -1;
    }

    memcpy(buf, (uint8_t *)read_bounce + (off - aligned_off), len);

    return (int)len;
}

int platform_flash_erase_sector(uint32_t off)
{
    static rom_erase_sector_fn rom_erase =
        (rom_erase_sector_fn)(uintptr_t)ROM_SPIFLASH_ERASE_SECTOR;

    uint32_t sector = off / FLASH_SECTOR_SIZE;
    uint32_t flags;
    int ret;

    if (!flash_ready) return -1;

    flags = irq_save();
    ret = rom_erase(sector);
    irq_restore(flags);

    return ret == 0 ? 0 : -1;
}

int platform_flash_program_bytes(uint32_t off, const void *buf, size_t len)
{
    static rom_write_fn rom_write =
        (rom_write_fn)(uintptr_t)ROM_SPIFLASH_WRITE;

    uint32_t flags;
    int ret;

    if (!flash_ready || buf == NULL) return -1;
    if ((off & 3u) || (len & 3u)) {
        log_warn("flash: program requires word alignment (%08x+%u)",
                 off, (unsigned)len);
        return -1;
    }

    flags = irq_save();
    ret = rom_write(off, (const uint32_t *)buf, (uint32_t)len);
    irq_restore(flags);

    return ret == 0 ? (int)len : -1;
}

/* No packrom-style embedded payload on this platform yet: init_process()
 * logs a warning and skips spawning the controller SEF. */
int platform_payload_controller(const uint8_t **data, uint32_t *size)
{
    (void)data; (void)size;
    return -1;
}
