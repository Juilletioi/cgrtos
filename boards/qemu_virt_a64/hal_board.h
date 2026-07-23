/**
 * @file boards/qemu_virt_a64/hal_board.h
 * @brief QEMU virt AArch64 板级 MMIO（PL011 + GICv3 + RAM）
 */
#ifndef HAL_BOARD_QEMU_VIRT_A64_H
#define HAL_BOARD_QEMU_VIRT_A64_H

#define HAL_BOARD_NAME              "qemu_virt_a64"

/* DRAM */
#define HAL_BOARD_RAM_BASE          0x40000000ULL
#define HAL_BOARD_RAM_SIZE          (256ULL * 1024 * 1024)

/* PL011 UART0 */
#define HAL_BOARD_UART0_BASE        0x09000000ULL
#define HAL_BOARD_UART_BASE         HAL_BOARD_UART0_BASE
#define HAL_BOARD_UART_KIND_PL011   1
#define HAL_BOARD_UART_IRQ          33
/* GICv3 PPI: virtual timer INTID */
#define HAL_BOARD_TIMER_IRQ         27

/* GICv3 (QEMU virt defaults) */
#define HAL_BOARD_GICD_BASE         0x08000000ULL
#define HAL_BOARD_GICR_BASE         0x080A0000ULL

/* Unused RISC-V symbols — keep HAL compile paths quiet if referenced */
#define HAL_BOARD_CLINT_BASE        0
#define HAL_BOARD_PLIC_BASE         0
#define HAL_BOARD_PLIC_PRIORITY_BASE 0
#define HAL_BOARD_PLIC_PENDING_BASE  0
#define HAL_BOARD_PLIC_ENABLE_BASE   0
#define HAL_BOARD_PLIC_THRESHOLD_BASE 0
#define HAL_BOARD_PLIC_CLAIM_BASE    0
#define HAL_BOARD_UART_KIND_SIFIVE  0
#define HAL_BOARD_UART_KIND_NS16550 0

#ifndef CONFIG_NUCLEI_MCACHE
#define CONFIG_NUCLEI_MCACHE        0
#endif

static inline void board_ipi_clear(unsigned cpu)
{
    (void)cpu;
}

#endif
