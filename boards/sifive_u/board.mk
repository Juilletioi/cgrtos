# boards/sifive_u/board.mk — QEMU sifive_u（SiFive U54 类）
# CPU=sifive-u54

BOARD_ARCH          := riscv
BOARD_UART_DRV      := uart.c
BOARD_LINK_SCRIPT   := boards/sifive_u/link.lds
BOARD_CFLAGS        := -DCONFIG_NUCLEI_MCACHE=0
BOARD_TIMER_HZ      := 10000000

BOARD_QEMU          ?= /usr/bin/qemu-system-riscv64
BOARD_QEMU_MACHINE  := sifive_u
BOARD_QEMU_CPU      ?= sifive-u54
BOARD_QEMU_CPU_EXT  :=
BOARD_QEMU_MEM      := 256M
BOARD_QEMU_LOAD     := elf
BOARD_QEMU_ENTRY    := 0x80000000
BOARD_QEMU_NEED_LOADER := 0
# sifive_u QEMU machine requires at least 2 CPUs
BOARD_QEMU_MIN_CORES := 2
