/**
 * @file boards/riscv_virt/hal_board.h
 * @brief QEMU RISC-V virt 板级 MMIO（通用 rv64 / 接近 SiFive 外设布局的 CLINT+PLIC）
 * @author Cong Zhou / Juilletioi
 * @version 5.2.0
 * @date 2026-07-23
 * @copyright CG-RTOS
 *
 * @details
 * - UART: NS16550 @ 0x10000000（非 SiFive UART）
 * - CLINT @ 0x02000000（mtime @ +0xBFF8）
 * - PLIC @ 0x0C000000
 * - DRAM @ 0x80000000
 * QEMU: -M virt -cpu rv64|sifive-u54 -bios none -kernel cgrtos.elf
 */
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

#include <stdint.h>

#define HAL_BOARD_UART_KIND_NS16550 1

#ifndef HAL_BOARD_UART_BASE
#define HAL_BOARD_UART_BASE       0x10000000UL
#endif
/* NS16550 寄存器偏移（字节） */
#define HAL_BOARD_UART_RBR        0x00U
#define HAL_BOARD_UART_THR        0x00U
#define HAL_BOARD_UART_IER        0x01U
#define HAL_BOARD_UART_FCR        0x02U
#define HAL_BOARD_UART_LCR        0x03U
#define HAL_BOARD_UART_MCR        0x04U
#define HAL_BOARD_UART_LSR        0x05U

#ifndef HAL_BOARD_CLINT_BASE
#define HAL_BOARD_CLINT_BASE      0x02000000UL
#endif
/* virt：mtime 在 CLINT 内，无独立 SysTimer */
#define HAL_BOARD_SYSTIMER_BASE   HAL_BOARD_CLINT_BASE
#define HAL_BOARD_MTIME_ADDR      (HAL_BOARD_CLINT_BASE + 0xBFF8UL)
#define HAL_BOARD_MTIMECMP_ADDR(h) \
    (HAL_BOARD_CLINT_BASE + 0x4000UL + (unsigned long)(h) * 8UL)
#define HAL_BOARD_CLINT_MSIP(h) \
    (HAL_BOARD_CLINT_BASE + (unsigned long)(h) * 4UL)

#ifndef HAL_BOARD_PLIC_BASE
#define HAL_BOARD_PLIC_BASE       0x0C000000UL
#endif
#define HAL_BOARD_PLIC_CTX_STRIDE    0x1000UL
#define HAL_BOARD_PLIC_ENABLE_STRIDE 0x80UL
#define HAL_BOARD_PLIC_M_CONTEXT(h) ((uint64_t)(h) * 2ULL)
#define HAL_BOARD_PLIC_PRIORITY(irq) \
    (HAL_BOARD_PLIC_BASE + 4ULL * (uint64_t)(irq))
#define HAL_BOARD_PLIC_ENABLE(h, word) \
    (HAL_BOARD_PLIC_BASE + 0x2000ULL + \
     HAL_BOARD_PLIC_M_CONTEXT(h) * HAL_BOARD_PLIC_ENABLE_STRIDE + \
     4ULL * (uint64_t)(word))
#define HAL_BOARD_PLIC_THRESHOLD(h) \
    (HAL_BOARD_PLIC_BASE + 0x200000UL + \
     HAL_BOARD_PLIC_M_CONTEXT(h) * HAL_BOARD_PLIC_CTX_STRIDE)
#define HAL_BOARD_PLIC_CLAIM(h) \
    (HAL_BOARD_PLIC_BASE + 0x200004UL + \
     HAL_BOARD_PLIC_M_CONTEXT(h) * HAL_BOARD_PLIC_CTX_STRIDE)

#ifndef HAL_BOARD_NAME
#define HAL_BOARD_NAME            "riscv_virt"
#endif

#define CONFIG_NUCLEI_MCACHE      0

#endif /* HAL_BOARD_H */
