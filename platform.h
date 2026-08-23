#ifndef SCORPION_PLATFORM_H
#define SCORPION_PLATFORM_H

/*
 * Platform abstraction interface.
 *
 * Everything hardware-specific lives behind this header. The generic
 * kernel (scheduler, allocator, loader, FUSE, syscalls) only ever calls
 * these hooks, so porting Scorpion to a new chip means implementing this
 * contract plus a linker script — see docs/porting.md.
 *
 * Select the platform at configure time:
 *   cmake -DPLATFORM=rp2350    (Raspberry Pi Pico 2, Hazard3 RV32IMAC(B))
 *   cmake -DPLATFORM=esp32c3   (ESP32-C3, RV32IMC, experimental)
 *   cmake -DPLATFORM=esp32s3   (ESP32-S3, Xtensa LX7, call0 ABI)
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(PLATFORM_RP2350)
    #define PLATFORM_NAME       "rp2350"
    /*
     * mcause value that identifies the machine-timer interrupt on this
     * platform (Hazard3 implements the standard RISC-V mtip / code 7).
     */
    #define PLATFORM_TIMER_MCAUSE (0x80000000u | 7u)
    /*
     * Device offset where the FUSE storage region begins. Must sit
     * above the boot image AND the controller payload that packrom.py
     * places at 0x10000, or formatting the filesystem would clobber
     * them.
     */
    #define FLASH_STORAGE_OFFSET 0x00020000u   /* 128 KB */
#elif defined(PLATFORM_ESP32C3)
    #define PLATFORM_NAME       "esp32c3"
    /*
     * ESP32-C3 has no CLINT: the SYSTIMER target0 compare fires through
     * the INTC as machine EXTERNAL interrupt (mcause bit31 set).
     * Source 17 = ETS_SYSTIMER_TARGET0_INTR_SOURCE on the C3 — verify
     * against soc/interrupts.h when porting to C6/H2/P4.
     */
    #define PLATFORM_TIMER_MCAUSE (0x80000000u | 17u)
    /* The kernel image itself occupies flash from 0x0 (~32 KB); keep
     * FUSE clear of it. */
    #define FLASH_STORAGE_OFFSET 0x00010000u   /* 64 KB */
#elif defined(PLATFORM_ESP32S3)
    #define PLATFORM_NAME       "esp32s3"
    /*
     * The S3 has no RISC-V mcause; trap entry assembly synthesizes
     * 0x80000000 | <CPU interrupt number> for interrupts. Scorpion uses
     * CPU interrupt 2 exclusively for the SYSTIMER tick.
     */
    #define PLATFORM_TIMER_MCAUSE (0x80000000u | 2u)
    /* Same layout rule as the C3: image at 0x0, FUSE above 64 KB. */
    #define FLASH_STORAGE_OFFSET 0x00010000u   /* 64 KB */
#else
    #error "Scorpion: define PLATFORM_RP2350, PLATFORM_ESP32C3 or PLATFORM_ESP32S3"
#endif

/* ---- Console ------------------------------------------------------- */

void platform_console_init(void);
void platform_uart_putc(char c);

/* ---- Flash ----------------------------------------------------------
 * Byte-offset primitives over the raw flash chip (offset 0 = start of
 * the flash device). Erase granularity is one 4 KB sector. Program may
 * only clear bits; callers do read-modify-write above this layer.
 */

int platform_flash_init(void);
int platform_flash_read_bytes(uint32_t off, void *buf, size_t len);
int platform_flash_erase_sector(uint32_t off);
int platform_flash_program_bytes(uint32_t off, const void *buf, size_t len);

/* ---- Timer -----------------------------------------------------------
 * A monotonically increasing free-running counter plus one absolute
 * comparator that raises PLATFORM_TIMER_MCAUSE when it matches.
 */

uint64_t platform_timer_now(void);
void     platform_timer_init(void);
void     platform_timer_arm(uint64_t deadline);
bool     platform_timer_is_irq(uintptr_t mcause);

/* ---- SMP / application payload -------------------------------------- */

void smp_launch_core1(void (*entry)(void), uintptr_t sp);
void core1_main(void);

/*
 * Locates the embedded controller SEF payload placed next to the kernel
 * image by tools/packrom.py. Returns 0 and fills size/data on success,
 * -1 when the platform has no embedded payload.
 */
int platform_payload_controller(const uint8_t **data, uint32_t *size);

#endif /* SCORPION_PLATFORM_H */
