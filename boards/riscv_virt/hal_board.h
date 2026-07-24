/**
 * @file boards/riscv_virt/hal_board.h
 * @brief QEMU RISC-V virt 板级 MMIO（通用 rv64 / 接近 SiFive 外设布局的 CLINT+PLIC）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
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

/**
 * @brief 本板 UART 为 NS16550 风格
 * @warning 无运行时副作用（编译期选型常量）
 */
#define HAL_BOARD_UART_KIND_NS16550 1

#ifndef HAL_BOARD_UART_BASE
/**
 * @brief UART0 MMIO 基址
 * @warning 无运行时副作用（地址常量）；错误基址会导致静默写错外设
 */
#define HAL_BOARD_UART_BASE       0x10000000UL
#endif
/**
 * @brief NS16550 RBR（接收缓冲）相对偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_RBR        0x00U
/**
 * @brief NS16550 THR（发送保持）相对偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_THR        0x00U
/**
 * @brief NS16550 IER（中断使能）相对偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_IER        0x01U
/**
 * @brief NS16550 FCR（FIFO 控制）相对偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_FCR        0x02U
/**
 * @brief NS16550 LCR（线路控制）相对偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_LCR        0x03U
/**
 * @brief NS16550 MCR（调制解调控制）相对偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_MCR        0x04U
/**
 * @brief NS16550 LSR（线路状态）相对偏移
 * @warning 仅计算偏移，无运行时副作用
 */
#define HAL_BOARD_UART_LSR        0x05U

#ifndef HAL_BOARD_CLINT_BASE
/**
 * @brief CLINT MMIO 基址（含 mtime/mtimecmp/MSIP）
 * @warning 无运行时副作用（地址常量）
 */
#define HAL_BOARD_CLINT_BASE      0x02000000UL
#endif
/**
 * @brief SysTimer 基址别名（virt 上等同 CLINT）
 * @warning 仅为别名，无额外运行时副作用
 */
#define HAL_BOARD_SYSTIMER_BASE   HAL_BOARD_CLINT_BASE
/**
 * @brief 自由运行 mtime 寄存器绝对地址（CLINT+0xBFF8）
 * @warning 仅计算地址，无运行时副作用
 */
#define HAL_BOARD_MTIME_ADDR      (HAL_BOARD_CLINT_BASE + 0xBFF8UL)
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
#define HAL_BOARD_PLIC_BASE       0x0C000000UL
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
#define HAL_BOARD            "riscv_virt"
#endif

/**
 * @brief 关闭 Nuclei mcache 路径（本板非 Nuclei）
 * @warning 无运行时副作用（编译期开关）
 */
#define CONFIG_NUCLEI_MCACHE      0

#endif /* HAL_BOARD_H */
