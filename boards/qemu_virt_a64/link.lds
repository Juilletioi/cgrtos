/* QEMU virt AArch64 — RAM at 0x40000000 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 256M
}

SECTIONS
{
    . = 0x40000000;
    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    } > RAM

    .rodata : {
        *(.rodata .rodata.*)
    } > RAM

    .data : {
        *(.data .data.*)
    } > RAM

    .bss : {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        _ebss = .;
    } > RAM

    . = ALIGN(16);
    _stack_bottom = .;
    . = . + (16 * 1024 * 4); /* 4 cores * 16KiB */
    _stack_top = .;

    _end = .;
}
