OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
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

    _stack_top = ORIGIN(SRAM) + LENGTH(SRAM);
    _heap_start = ADDR(.bss) + SIZEOF(.bss);
    _heap_end = ORIGIN(SRAM) + LENGTH(SRAM) - 16K;

    _flash_data = LOADADDR(.data);
}
