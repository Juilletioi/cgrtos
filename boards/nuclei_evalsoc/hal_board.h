/**
 * @file boards/nuclei_evalsoc/hal_board.h
 * @brief Nuclei evalsoc（UX/NX 系列）板级 MMIO
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details QEMU: -M nuclei_evalsoc,download=ddr -cpu nuclei-ux900fd|nx900fd|…
 *          适用于芯来 UX900 / NX900 / UX600 / NX600 等 evalsoc 仿真配置。
 */
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

#include <stdint.h>

/**
 * @brief 本板 UART 为 SiFive 兼容风格（非 NS16550）
 * @warning 无运行时副作用（编译期选型常量）
 */
#define HAL_BOARD_UART_KIND_SIFIVE  1

#ifndef HAL_BOARD_UART_BASE
/**
 * @brief UART0 MMIO 基址
 * @warning 无运行时副作用（地址常量）；错误基址会导致静默写错外设
 */
#define HAL_BOARD_UART_BASE       0x10013000UL
#endif
/**
 * @brief UART TXDATA 寄存器相对基址偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_TXDATA     0x00U
/**
 * @brief UART RXDATA 寄存器相对基址偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_RXDATA     0x04U
/**
 * @brief UART TXCTRL 寄存器相对基址偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_TXCTRL     0x08U
/**
 * @brief UART RXCTRL 寄存器相对基址偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_RXCTRL     0x0CU

#ifndef HAL_BOARD_SYSTIMER_BASE
/**
 * @brief Nuclei SysTimer（mtime）MMIO 基址
 * @warning 无运行时副作用（地址常量）
 */
#define HAL_BOARD_SYSTIMER_BASE   0x18030000UL
#endif
#ifndef HAL_BOARD_CLINT_BASE
/**
 * @brief CLINT（MSIP/mtimecmp）MMIO 基址
 * @warning 无运行时副作用（地址常量）
 */
#define HAL_BOARD_CLINT_BASE      0x18031000UL
#endif

/**
 * @brief 自由运行 mtime 寄存器绝对地址
 * @warning 仅计算地址，无运行时副作用
 */
#define HAL_BOARD_MTIME_ADDR      (HAL_BOARD_SYSTIMER_BASE + 0x0UL)
/**
 * @brief 指定 hart 的 mtimecmp 绝对地址
 * @warning 仅计算地址，无运行时副作用；h 越界时地址非法
 */
#define HAL_BOARD_MTIMECMP_ADDR(h) \
    (HAL_BOARD_CLINT_BASE + 0x4000UL + (unsigned long)(h) * 8UL)
/**
 * @brief 指定 hart 的 MSIP 绝对地址
 * @warning 仅计算地址，无运行时副作用；h 越界时地址非法
 */
#define HAL_BOARD_CLINT_MSIP(h) \
    (HAL_BOARD_CLINT_BASE + (unsigned long)(h) * 4UL)

#ifndef HAL_BOARD_PLIC_BASE
/**
 * @brief PLIC MMIO 基址
 * @warning 无运行时副作用（地址常量）
 */
#define HAL_BOARD_PLIC_BASE       0x1C000000UL
#endif
/**
 * @brief PLIC 每 context 寄存器区步长
 * @warning 无运行时副作用（布局常量）
 */
#define HAL_BOARD_PLIC_CTX_STRIDE    0x1000UL
/**
 * @brief PLIC 每 context enable 区步长
 * @warning 无运行时副作用（布局常量）
 */
#define HAL_BOARD_PLIC_ENABLE_STRIDE 0x80UL
/**
 * @brief 将 hart 映射为 M-mode PLIC context 编号（hart*2）
 * @warning 仅计算编号，无运行时副作用
 */
#define HAL_BOARD_PLIC_M_CONTEXT(h) ((uint64_t)(h) * 2ULL)
/**
 * @brief 中断源 priority 寄存器绝对地址
 * @warning 仅计算地址，无运行时副作用
 */
#define HAL_BOARD_PLIC_PRIORITY(irq) \
    (HAL_BOARD_PLIC_BASE + 4ULL * (uint64_t)(irq))
/**
 * @brief 指定 hart/字的 enable 寄存器绝对地址
 * @warning 仅计算地址，无运行时副作用；h/word 越界时地址非法
 */
#define HAL_BOARD_PLIC_ENABLE(h, word) \
    (HAL_BOARD_PLIC_BASE + 0x2000ULL + \
     HAL_BOARD_PLIC_M_CONTEXT(h) * HAL_BOARD_PLIC_ENABLE_STRIDE + \
     4ULL * (uint64_t)(word))
/**
 * @brief 指定 hart 的 threshold 寄存器绝对地址
 * @warning 仅计算地址，无运行时副作用
 */
#define HAL_BOARD_PLIC_THRESHOLD(h) \
    (HAL_BOARD_PLIC_BASE + 0x200000UL + \
     HAL_BOARD_PLIC_M_CONTEXT(h) * HAL_BOARD_PLIC_CTX_STRIDE)
/**
 * @brief 指定 hart 的 claim/complete 寄存器绝对地址
 * @warning 仅计算地址，无运行时副作用
 */
#define HAL_BOARD_PLIC_CLAIM(h) \
    (HAL_BOARD_PLIC_BASE + 0x200004UL + \
     HAL_BOARD_PLIC_M_CONTEXT(h) * HAL_BOARD_PLIC_CTX_STRIDE)

#ifndef HAL_BOARD
/**
 * @brief 板级短名字符串（诊断/打印）
 * @warning 无运行时副作用（字符串常量）
 */
#define HAL_BOARD            "nuclei_evalsoc"
#endif

#ifndef CONFIG_NUCLEI_MCACHE
/**
 * @brief 启用 Nuclei 机器模式 cache 相关路径（本板默认 1）
 * @warning 无运行时副作用（编译期开关）；改值需与 CPU 能力一致
 */
#define CONFIG_NUCLEI_MCACHE      1
#endif

#endif /* HAL_BOARD_H */
