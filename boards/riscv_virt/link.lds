/* cgrtos - RISC-V virt RTOS Linker Script */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    ram   (wxa!ri) : ORIGIN = 0x80000000, LENGTH = 128M
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
     * clears them and secondary boot poll can race on garbage / wrong layout.
     */
    .bss : ALIGN(16) {
        _sbss = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } >ram

    /*
     * Per-hart boot stacks (16 KiB each). Always reserve CONFIG_MAX_CORES=4 slots
     * so the same image layout works for CORES=1/2/4; unused harts simply idle.
     * Stack top for hart N = _stack_top + N * 0x4000 (see startup.S).
     */
    .stack : ALIGN(16) {
        . += 0x4000;
        _stack_top = .;          /* hart0 */
        . += 0x4000;
        _stack_top_core1 = .;    /* hart1 (compat symbol) */
        . += 0x4000;
        _stack_top_core2 = .;    /* hart2 */
        . += 0x4000;
        _stack_top_core3 = .;    /* hart3 */
    } >ram

    _end = .;
}
