# boards/nuclei_evalsoc/board.mk — 芯来 evalsoc（UX900 / NX900 / UX600 …）
# CPU=nuclei-ux900fd|nuclei-nx900fd|nuclei-nx600fd|nuclei-ux600fd|…

BOARD_ARCH          := riscv
BOARD_UART_DRV      := uart.c
BOARD_LINK_SCRIPT   := boards/nuclei_evalsoc/link.lds
BOARD_CFLAGS        := -DCONFIG_NUCLEI_MCACHE=1
BOARD_TIMER_HZ      := 1000000

BOARD_QEMU          ?= /home/cong/nuclei-ux900fd-linux/tools/nuclei-qemu/bin/qemu-system-riscv64
BOARD_QEMU_MACHINE  := nuclei_evalsoc,download=ddr
BOARD_QEMU_CPU      ?= nuclei-ux900fd
BOARD_QEMU_CPU_EXT  := ,ext=svpbmt_zicbom_sstc_sscofpmf_zba_zbb_zbc_zbs_zicond
BOARD_QEMU_MEM      := 512M
BOARD_QEMU_LOAD     := bios_bin
BOARD_QEMU_ENTRY    := 0xA0000000
BOARD_QEMU_NEED_LOADER := 1
