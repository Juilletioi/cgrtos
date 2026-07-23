/**
 * @file boards/sifive_u/hal_board.h
 * @brief QEMU sifive_u（SiFive U54 类）板级 MMIO
 * @author Cong Zhou / Juilletioi
 * @version 5.2.0
 * @date 2026-07-23
 * @copyright CG-RTOS
 *
 * @details QEMU: -M sifive_u -cpu sifive-u54 -bios none -kernel cgrtos.elf
 *          UART0 SiFive @ 0x10010000；CLINT/PLIC 与 virt 同类布局。
 */
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

#include <stdint.h>

#define HAL_BOARD_UART_KIND_SIFIVE  1

#ifndef HAL_BOARD_UART_BASE
#define HAL_BOARD_UART_BASE       0x10010000UL
#endif
#define HAL_BOARD_UART_TXDATA     0x00U
#define HAL_BOARD_UART_RXDATA     0x04U
#define HAL_BOARD_UART_TXCTRL     0x08U
#define HAL_BOARD_UART_RXCTRL     0x0CU

#ifndef HAL_BOARD_CLINT_BASE
#define HAL_BOARD_CLINT_BASE      0x02000000UL
#endif
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
#define HAL_BOARD_NAME            "sifive_u"
#endif

#define CONFIG_NUCLEI_MCACHE      0

#endif /* HAL_BOARD_H */
