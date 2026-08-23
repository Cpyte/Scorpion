OUTPUT_ARCH(xtensa)
ENTRY(_start)

/*
 * ESP32-S3 layout: Scorpion runs entirely from RAM. The boot ROM
 * validates the image at flash offset 0x0 and copies every segment to
 * its VMA itself (LMA == VMA in this script), then enters _start — no
 * second-stage bootloader, same model as the ESP32-C3 port.
 *
 *   IRAM  0x40370000  vector table + .text/.rodata (executed in place)
 *   DRAM  0x3FC90000  .data/.bss + heap ...        (ROM loads .data)
 *
 * DRAM stops at 0x3FCE5000: the ROM keeps a handful of .data variables
 * around 0x3FCEFFxx–0x3FF1FFxx (ets_ops_table_ptr etc.) that we must
 * not overlap.
 */

MEMORY
{
    IRAM (rx)  : ORIGIN = 0x40370000, LENGTH = 448K
    DRAM (rwx) : ORIGIN = 0x3FC90000, LENGTH = 344K
}

SECTIONS
{
    /* The vector table must sit at the start of IRAM so that VECBASE
     * can simply be ORIGIN(IRAM): it is 4 KB aligned by vectors.S. */
    .text : {
        KEEP(*(.vec.table))
        *(.text.trap)
        *(.text._start)
        *(.text.context_switch)
        *(.text.ecall_trigger)
        *(.literal)
        *(.literal.*)
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
