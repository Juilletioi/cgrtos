# boards/qemu_virt_a64/board.mk — QEMU virt AArch64 (A53≈A55 / A72≈A75)
#
# CPU aliases (QEMU 6.2 has no cortex-a55/a75):
#   cortex-a53  — A55-class
#   cortex-a72  — A75-class

BOARD_ARCH          := arm64
BOARD_UART_DRV      := uart_pl011.c
BOARD_LINK_SCRIPT   := boards/qemu_virt_a64/link.lds
BOARD_CFLAGS        := -DCONFIG_NUCLEI_MCACHE=0 -DCONFIG_DELAY_BUSY_US=0
BOARD_TIMER_HZ      := 62500000

BOARD_CROSS         ?= aarch64-linux-gnu-
BOARD_SYSROOT       ?=
BOARD_QEMU          ?= /home/cong/cgrtos-tools/root/usr/bin/qemu-system-aarch64
BOARD_QEMU_MACHINE  := virt,gic-version=3
BOARD_QEMU_CPU      ?= cortex-a53
BOARD_QEMU_CPU_EXT  :=
BOARD_QEMU_MEM      := 256M
BOARD_QEMU_LOAD     := kernel
BOARD_QEMU_ENTRY    := 0x40000000
BOARD_QEMU_NEED_LOADER := 0
BOARD_QEMU_MAX_CORES := 1
