/**
 * @file hal_board.h
 * @brief Nuclei UX900 / evalsoc 板级 MMIO 地址表（HAL 板级配置）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 所有外设驱动应通过本头文件获取基址，禁止在驱动源文件中散落硬编码常量。
 * 移植到其他 SoC 时：复制本文件为新板级头，或用 `-DHAL_BOARD_xxx=` 覆盖。
 * 本头仅含地址与布局宏，不含运行时状态；可被多编译单元只读包含。
 */
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Console UART                                                                */
/* -------------------------------------------------------------------------- */

#ifndef HAL_BOARD_UART_BASE
/** @brief UART0 MMIO 基址 */
#define HAL_BOARD_UART_BASE       0x10013000UL
#endif
/** @brief TXDATA 偏移 */
#define HAL_BOARD_UART_TXDATA     0x00U
/** @brief RXDATA 偏移 */
#define HAL_BOARD_UART_RXDATA     0x04U
/** @brief TXCTRL 偏移 */
#define HAL_BOARD_UART_TXCTRL     0x08U
/** @brief RXCTRL 偏移 */
#define HAL_BOARD_UART_RXCTRL     0x0CU

/* -------------------------------------------------------------------------- */
/* System Timer + CLINT                                                        */
/* -------------------------------------------------------------------------- */

#ifndef HAL_BOARD_SYSTIMER_BASE
/** @brief SysTimer（mtime）基址 */
#define HAL_BOARD_SYSTIMER_BASE   0x18030000UL
#endif
#ifndef HAL_BOARD_CLINT_BASE
/** @brief CLINT（MSIP / mtimecmp）基址 */
#define HAL_BOARD_CLINT_BASE      0x18031000UL
#endif

/** @brief mtime 寄存器绝对地址 */
#define HAL_BOARD_MTIME_ADDR      (HAL_BOARD_SYSTIMER_BASE + 0x0UL)

/**
 * @brief 指定 hart 的 mtimecmp 地址
 * @details 计算 CLINT 内 mtimecmp 槽绝对地址。
 * @param h hart 编号（0..CONFIG_MAX_CORES-1）
 * @warning 宏参数 h 只应出现一次求值安全的表达式；勿传入带副作用的表达式
 */
#define HAL_BOARD_MTIMECMP_ADDR(h) \
    (HAL_BOARD_CLINT_BASE + 0x4000UL + (unsigned long)(h) * 8UL)

/**
 * @brief 指定 hart 的 MSIP 地址（软件 IPI）
 * @details 计算 CLINT MSIP 字地址。
 * @param h hart 编号
 * @warning 勿对 h 使用 ++ 等副作用表达式（多次求值风险）
 */
#define HAL_BOARD_CLINT_MSIP(h) \
    (HAL_BOARD_CLINT_BASE + (unsigned long)(h) * 4UL)

/* -------------------------------------------------------------------------- */
/* PLIC                                                                        */
/* -------------------------------------------------------------------------- */

#ifndef HAL_BOARD_PLIC_BASE
/** @brief PLIC 基址 */
#define HAL_BOARD_PLIC_BASE       0x1C000000UL
#endif

/** @brief 每 context 的 threshold/claim 块跨度 */
#define HAL_BOARD_PLIC_CTX_STRIDE    0x1000UL
/** @brief 每 context 的 enable 块跨度 */
#define HAL_BOARD_PLIC_ENABLE_STRIDE 0x80UL

/**
 * @brief Nuclei evalsoc：M-mode context = hart * 2（中间夹 S-mode context）
 * @param h hart 编号
 */
#define HAL_BOARD_PLIC_M_CONTEXT(h) ((uint64_t)(h) * 2ULL)

/** @brief 源优先级寄存器 */
#define HAL_BOARD_PLIC_PRIORITY(irq) \
    (HAL_BOARD_PLIC_BASE + 4ULL * (uint64_t)(irq))

/**
 * @brief 本 hart M-mode enable 字
 * @param h    hart
 * @param word 字索引 = irq/32
 */
#define HAL_BOARD_PLIC_ENABLE(h, word) \
    (HAL_BOARD_PLIC_BASE + 0x2000ULL + \
     HAL_BOARD_PLIC_M_CONTEXT(h) * HAL_BOARD_PLIC_ENABLE_STRIDE + \
     4ULL * (uint64_t)(word))

/** @brief 本 hart M-mode 优先级阈值 */
#define HAL_BOARD_PLIC_THRESHOLD(h) \
    (HAL_BOARD_PLIC_BASE + 0x200000UL + \
     HAL_BOARD_PLIC_M_CONTEXT(h) * HAL_BOARD_PLIC_CTX_STRIDE)

/** @brief 本 hart M-mode claim/complete（同址读写） */
#define HAL_BOARD_PLIC_CLAIM(h) \
    (HAL_BOARD_PLIC_BASE + 0x200004UL + \
     HAL_BOARD_PLIC_M_CONTEXT(h) * HAL_BOARD_PLIC_CTX_STRIDE)

/** @brief 板级名称（诊断 / SDK） */
#define HAL_BOARD_NAME            "nuclei_evalsoc_ux900"

#endif /* HAL_BOARD_H */
