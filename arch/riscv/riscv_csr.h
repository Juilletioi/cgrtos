/**
 * @file riscv_csr.h
 * @brief RISC-V CSR 读写宏（仅 arch/riscv 与遗留兼容路径）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * - 提供 read_csr / write_csr / set_csr_bits / clear_csr_bits。
 * - 新内核代码请用 arch_port.h；勿在业务层直接碰 CSR。
 * - 仅在 __riscv 工具链下可编译。
 */
#ifndef CGRTOS_RISCV_CSR_H
#define CGRTOS_RISCV_CSR_H

#if !defined(__riscv)
#error "riscv_csr.h is RISC-V only"
#endif

#include <stdint.h>

/**
 * @brief 读指定 CSR 寄存器
 * @param reg CSR 符号名（如 mstatus、mie；由预处理器字符串化）
 * @return 该 CSR 的 64 位当前值
 * @warning 非法 CSR 名会导致汇编失败；读副作用 CSR 可能改变硬件状态
 * @note 展开为 csrr 内联汇编语句表达式
 */
#define read_csr(reg) ({ \
    uint64_t __v; \
    __asm__ volatile("csrr %0, " #reg : "=r"(__v)); \
    __v; \
})

/**
 * @brief 写指定 CSR 寄存器
 * @param reg CSR 符号名
 * @param val 写入值（强制为 uint64_t）
 * @warning 覆盖整个 CSR；误写 mie/mstatus 等可破坏中断与特权状态
 * @note 展开为 csrw 内联汇编
 */
#define write_csr(reg, val) \
    __asm__ volatile("csrw " #reg ", %0" :: "r"((uint64_t)(val)))

/**
 * @brief 对指定 CSR 按位置 1（RMW：先读后写）
 * @param reg  CSR 符号名
 * @param bits 待置位的掩码
 * @warning 非原子 RMW；中断嵌套下并发写同一 CSR 可能丢更新
 * @note 等价于 write_csr(reg, read_csr(reg) | bits)
 */
#define set_csr_bits(reg, bits) ({ \
    uint64_t __v = read_csr(reg); \
    __v |= (bits); \
    write_csr(reg, __v); \
})

/**
 * @brief 对指定 CSR 按位清 0（RMW：先读后写）
 * @param reg  CSR 符号名
 * @param bits 待清除的掩码
 * @warning 非原子 RMW；清 mstatus.MIE 等会影响全局中断使能
 * @note 等价于 write_csr(reg, read_csr(reg) & ~bits)
 */
#define clear_csr_bits(reg, bits) ({ \
    uint64_t __v = read_csr(reg); \
    __v &= ~(bits); \
    write_csr(reg, __v); \
})

#endif /* CGRTOS_RISCV_CSR_H */
