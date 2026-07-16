/**
 * @file plic.c
 * @brief RISC-V PLIC 外部中断控制器驱动。
 *
 * 提供 PLIC 初始化、claim/complete 及外部中断与异常处理入口。
 *
 * Nuclei evalsoc DT 按 hart 提供 M+S 两套 context（mei + sei），因此
 * M-mode context 索引为 hart * 2（而非 hart * 1）。
 */

#include "../../kernel/cgrtos.h"

/** @brief PLIC 寄存器基址。 */
#define PLIC_BASE 0x1C000000
/** @brief 每 hart 的 context 跨度（M 与 S 各占一个）。 */
#define PLIC_CTX_STRIDE 0x1000
/**
 * @brief 指定 hart 的 M-mode PLIC context 编号。
 * @note evalsoc: contexts 0=hart0-M, 1=hart0-S, 2=hart1-M, 3=hart1-S。
 */
#define PLIC_M_CONTEXT(h) ((uint64_t)(h) * 2ULL)
/** @brief 指定 hart 的 PLIC 优先级阈值寄存器地址（M-mode）。 */
#define PLIC_THRESHOLD(h) (PLIC_BASE + 0x200000 + PLIC_M_CONTEXT(h) * PLIC_CTX_STRIDE)
/** @brief 指定 hart 的 PLIC claim/complete 寄存器地址（M-mode）。 */
#define PLIC_CLAIM(h) (PLIC_BASE + 0x200004 + PLIC_M_CONTEXT(h) * PLIC_CTX_STRIDE)

/**
 * @brief 初始化本 hart 的 PLIC 并使能 MEIE。
 *
 * @details
 * 1. 读取本 hart 编号 h。
 * 2. 将本 hart M-mode context 的优先级阈值寄存器置 0，允许所有优先级的外部中断通过。
 * 3. 通过 set_csr_bits 打开 mie 的 MEIE 位（bit 11），使能机器外部中断。
 */
void cgrtos_plic_init(void)
{
    /* 1. 读取本 hart 编号 */
    uint64_t h = read_csr(mhartid);
    /* 2. 阈值置 0 允许所有优先级中断 */
    volatile uint32_t *thr = (volatile uint32_t *)PLIC_THRESHOLD(h);
    *thr = 0;
    /* 3. 打开 MEIE 使能外部中断 */
    set_csr_bits(mie, 0x800);
}

/**
 * @brief 从 PLIC claim 寄存器读取待服务中断号。
 *
 * @return 中断 ID；0 表示无待处理中断。
 *
 * @details
 * 1. 读取本 hart 编号。
 * 2. 读取本 hart M-mode context 的 claim 寄存器。
 * 3. 返回 claim 值作为中断源 ID（0 表示无中断）。
 */
uint32_t cgrtos_plic_claim(void)
{
    /* 1. 读取本 hart 编号 */
    uint64_t h = read_csr(mhartid);
    /* 2-3. 读取 claim 并返回中断 ID */
    return *(volatile uint32_t *)PLIC_CLAIM(h);
}

/**
 * @brief 向 PLIC 完成指定中断的服务。
 *
 * @param irq 中断 ID（claim 返回值）。
 *
 * @details
 * 1. 读取本 hart 编号。
 * 2. 向 claim/complete 寄存器写入 irq，通知 PLIC 该中断已处理完毕。
 */
void cgrtos_plic_complete(uint32_t irq)
{
    /* 1. 读取本 hart 编号 */
    uint64_t h = read_csr(mhartid);
    /* 2. 写入 complete 通知 PLIC 中断已处理 */
    *(volatile uint32_t *)PLIC_CLAIM(h) = irq;
}

/**
 * @brief 机器外部中断 C 层入口。
 *
 * @param f 陷阱栈帧指针（未使用）。
 *
 * @details
 * 1. 调用 cgrtos_isr_enter 进入 ISR 上下文。
 * 2. 通过 cgrtos_plic_claim 读取待服务中断号。
 * 3. 若 irq 非 0，调用 cgrtos_plic_complete 完成中断服务。
 * 4. 调用 cgrtos_isr_exit 退出 ISR 上下文。
 *
 * @note 当前实现仅 claim/complete，未分发至具体设备 ISR。
 */
void riscv_handle_external(uint64_t *f)
{
    (void)f;
    /* 1. 进入 ISR 上下文 */
    cgrtos_isr_enter();
    /* 2. claim 待服务中断号 */
    uint32_t irq = cgrtos_plic_claim();
    /* 3. 非零则 complete 中断服务 */
    if (irq) {
        cgrtos_plic_complete(irq);
    }
    /* 4. 退出 ISR 上下文 */
    cgrtos_isr_exit();
}

/**
 * @brief 同步异常 C 层入口（非法指令等）。
 *
 * @param f     陷阱栈帧（未使用）。
 * @param cause mcause 异常原因编码。
 * @param epc   mepc 异常发生时的 PC。
 *
 * @details
 * 1. 接收 trap 向量传入的 cause 与 epc。
 * 2. 通过 cgrtos_printf 打印异常原因与 PC，供调试诊断。
 */
void riscv_handle_exception(uint64_t *f, uint64_t cause, uint64_t epc)
{
    (void)f;
    /* 1. 接收 cause 与 epc（已由 trap 向量传入） */
    /* 2. 打印异常信息供调试 */
    cgrtos_printf("[EXC] cause=%lu epc=%p\n",
                  (unsigned long)cause, (void *)(uintptr_t)epc);
}
