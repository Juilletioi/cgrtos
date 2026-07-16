/* cgrtos - Nuclei UX900 RTOS Linker Script */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    ram   (wxa!ri) : ORIGIN = 0xA0000000, LENGTH = 512M
}

SECTIONS
{
    .text : ALIGN(16) {
        _stext = .;
        KEEP(*(.text.init))
        *(.text .text.*)
        _etext = .;
    } >ram

    .rodata : ALIGN(16) {
        _srodata = .;
        *(.rodata .rodata.*)
        . = ALIGN(8);
        _erodata = .;
    } >ram

    .data : ALIGN(16) {
        _sdata = .;
        __global_pointer$ = . + 0x800;
        *(.data .data.*)
        . = ALIGN(8);
        _edata = .;
    } >ram

    /*
     * Merge .sbss into .bss. GCC places small zero-init globals (g_boot_sync,
     * g_sched_run, …) in .sbss; if we stop _ebss before .sbss, startup never
     * clears them and hart1's boot poll can race on garbage / wrong layout.
     */
    .bss : ALIGN(16) {
        _sbss = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } >ram

    .stack : ALIGN(16) {
        . += 0x4000;
        _stack_top = .;
        . += 0x4000;
        _stack_top_core1 = .;
    } >ram

    _end = .;
}
