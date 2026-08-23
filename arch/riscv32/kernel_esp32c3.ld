OUTPUT_ARCH(riscv)
ENTRY(_start)

/*
 * ESP32-C3 layout: Scorpion runs entirely from RAM and is loaded there
 * by the Espressif second-stage bootloader (see docs/platforms.md).
 *
 *   IRAM  0x40380000  .text/.rodata          (executed in place)
 *   DRAM  0x3FC90000  .data/.bss + heap ...  (bootloader loads .data)
 *
 * Because LMA == VMA for every section, crt0.S's "copy .data" step
 * copies the region onto itself — harmless — and .bss still gets
 * cleared normally.
 */

MEMORY
{
    IRAM (rx)  : ORIGIN = 0x40380000, LENGTH = 240K
    DRAM (rwx) : ORIGIN = 0x3FC90000, LENGTH = 384K
}

SECTIONS
{
    .text : {
        *(.text.trap)
        *(.text._start)
        *(.text)
        *(.text.*)
        . = ALIGN(4);
    } > IRAM

    .rodata : {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4);
    } > IRAM

    .data : {
        _data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4);
        _data_end = .;
    } > DRAM

    .bss : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > DRAM

    . = ALIGN(4096);
    _heap_start = .;
    _heap_end = ORIGIN(DRAM) + LENGTH(DRAM) - 16384 - 64K;
    __heap_start = _heap_start;
    __heap_end = _heap_end;
    _user_arena_start = _heap_end;
    _user_arena_end = _heap_end + 64K;
    _stack_top = ORIGIN(DRAM) + LENGTH(DRAM);

    _flash_data = LOADADDR(.data);
}
