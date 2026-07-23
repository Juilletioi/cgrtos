# boards/riscv_virt/board.mk — 通用 QEMU virt（rv64 / sifive-u54）
# CPU=rv64|sifive-u54

BOARD_ARCH          := riscv
BOARD_UART_DRV      := uart_ns16550.c
BOARD_LINK_SCRIPT   := boards/riscv_virt/link.lds
BOARD_CFLAGS        := -DCONFIG_NUCLEI_MCACHE=0
BOARD_TIMER_HZ      := 10000000

BOARD_QEMU          ?= /usr/bin/qemu-system-riscv64
BOARD_QEMU_MACHINE  := virt
BOARD_QEMU_CPU      ?= rv64
BOARD_QEMU_CPU_EXT  :=
BOARD_QEMU_MEM      := 128M
BOARD_QEMU_LOAD     := elf
BOARD_QEMU_ENTRY    := 0x80000000
BOARD_QEMU_NEED_LOADER := 0
