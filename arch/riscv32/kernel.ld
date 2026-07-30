OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 520K
}

SECTIONS
{
    .text : {
        *(.text.trap)
        *(.text._start)
        *(.text)
        *(.text.*)
        . = ALIGN(4);
    } > SRAM

    .rodata : {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4);
    } > SRAM

    .data : {
        _data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4);
        _data_end = .;
    } > SRAM

    .bss : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > SRAM

    . = ALIGN(4096);
    _heap_start = .;
    _heap_end = ORIGIN(SRAM) + LENGTH(SRAM) - 16384 - 256K;
    __heap_start = _heap_start;
    __heap_end = _heap_end;
    _user_arena_start = _heap_end;
    _user_arena_end = _heap_end + 256K;
    _stack_top = ORIGIN(SRAM) + LENGTH(SRAM);

    _flash_data = LOADADDR(.data);
}
