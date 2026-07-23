/**
 * @file riscv_csr.h
 * @brief RISC-V CSR 读写宏（仅 arch/riscv 与遗留兼容路径）
 * @details 新内核代码请用 arch_port.h；勿在业务层直接碰 CSR。
 */
#ifndef CGRTOS_RISCV_CSR_H
#define CGRTOS_RISCV_CSR_H

#if !defined(__riscv)
#error "riscv_csr.h is RISC-V only"
#endif

#include <stdint.h>

/** @def read_csr 读 CSR */
#define read_csr(reg) ({ \
    uint64_t __v; \
    __asm__ volatile("csrr %0, " #reg : "=r"(__v)); \
    __v; \
})

/** @def write_csr 写 CSR */
#define write_csr(reg, val) \
    __asm__ volatile("csrw " #reg ", %0" :: "r"((uint64_t)(val)))

/** @def set_csr_bits 置位 CSR 中若干 bit */
#define set_csr_bits(reg, bits) ({ \
    uint64_t __v = read_csr(reg); \
    __v |= (bits); \
    write_csr(reg, __v); \
})

/** @def clear_csr_bits 清除 CSR 中若干 bit */
#define clear_csr_bits(reg, bits) ({ \
    uint64_t __v = read_csr(reg); \
    __v &= ~(bits); \
    write_csr(reg, __v); \
})

#endif /* CGRTOS_RISCV_CSR_H */
