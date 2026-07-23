/**
 * @file arch_port.h
 * @brief CPU 移植层：核 ID、关中断、自愿让出、idle、IPI 使能、任务栈帧
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-23
 * @copyright CG-RTOS
 *
 * @details
 * 内核与 HAL 只应依赖本头中的 arch_* API，禁止在 kernel 源文件内直接写
 * CSR / DAIF / svc / ecall。新增架构时在本文件补齐对应条件编译分支即可。
 *
 * RISC-V: mhartid, mstatus.MIE, ecall, mie.MSIE
 * AArch64: MPIDR_EL1 Aff0, DAIF.I, svc #0, IPI no-op until SGI
 *
 * 注意：注释中勿写星号斜杠序列（会提前结束块注释）。
 */
#ifndef CGRTOS_ARCH_PORT_H
#define CGRTOS_ARCH_PORT_H

#include <stdint.h>

struct cgrtos_task; /* 前向声明 */

/**
 * @brief 当前逻辑核号（0 .. CONFIG_NUM_CORES-1）
 * @return 核号
 * @attention ✅ ISR；❌ 不阻塞
 */
static inline unsigned arch_cpu_id(void)
{
#if defined(__aarch64__)
    unsigned long mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (unsigned)(mpidr & 0xffu);
#elif defined(__riscv)
    unsigned long id;
    __asm__ volatile("csrr %0, mhartid" : "=r"(id));
    return (unsigned)(id & 0xffu);
#else
#error "unsupported architecture: need arch_cpu_id()"
#endif
}

/**
 * @brief 保存并屏蔽可屏蔽 IRQ，返回供 restore 的标志字
 * @return 架构相关 flags（mstatus 或 DAIF）
 * @attention ✅ ISR；❌ 不阻塞
 */
static inline uint64_t arch_irq_save(void)
{
#if defined(__aarch64__)
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2"); /* IRQ mask */
    return daif;
#elif defined(__riscv)
    uint64_t flags;
    __asm__ volatile("csrr %0, mstatus" : "=r"(flags));
    __asm__ volatile("csrci mstatus, 0x8"); /* clear MIE */
    return flags;
#else
#error "unsupported architecture: need arch_irq_save()"
#endif
}

/**
 * @brief 恢复 arch_irq_save 保存的中断屏蔽状态
 * @param[in] flags arch_irq_save 返回值
 * @attention ✅ ISR；❌ 不阻塞
 */
static inline void arch_irq_restore(uint64_t flags)
{
#if defined(__aarch64__)
    __asm__ volatile("msr daif, %0" :: "r"(flags));
#elif defined(__riscv)
    __asm__ volatile("csrw mstatus, %0" :: "r"(flags));
#else
#error "unsupported architecture: need arch_irq_restore()"
#endif
}

/**
 * @brief 自愿陷入（任务上下文请求调度）
 * @details 调用前须已置 g_yield_pending；返回后可能已切换到其它任务。
 * @note AArch64：ELR 已指向下一条指令，SVC 处理勿再加 4。
 * @attention ❌ ISR；✅ 可能切换
 */
static inline void arch_yield_trap(void)
{
#if defined(__aarch64__)
    __asm__ volatile("svc #0" ::: "memory");
#elif defined(__riscv)
    __asm__ volatile("ecall" ::: "memory");
#else
#error "unsupported architecture: need arch_yield_trap()"
#endif
}

/**
 * @brief 低功耗等待中断（idle / halt）
 * @attention ✅ ISR；❌ 不“阻塞等待事件”语义上的 sleep API
 */
static inline void arch_cpu_wait(void)
{
    __asm__ volatile("wfi" ::: "memory");
}

/**
 * @brief 使能本核核间中断接收（次核启动路径）
 * @details RISC-V 置 mie.MSIE；AArch64 当前为 no-op（无 SGI IPI）。
 * @attention ❌ ISR；❌ 不阻塞
 */
static inline void arch_cpu_enable_ipi(void)
{
#if defined(__riscv)
    unsigned long mie;
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    mie |= 0x8UL; /* MSIE */
    __asm__ volatile("csrw mie, %0" :: "r"(mie));
#elif defined(__aarch64__)
    /* GICv3 SGI IPI：后续 SMP 再开 */
#else
#error "unsupported architecture: need arch_cpu_enable_ipi()"
#endif
}

/**
 * @brief 初始化任务 trap/上下文栈帧（架构相关，见 arch 下 task_stack.c）
 * @param[in,out] task TCB
 * @param[in] fn 入口
 * @param[in] arg 参数（a0 / x0）
 * @attention ❌ ISR；❌ 不阻塞
 */
void arch_task_stack_init(struct cgrtos_task *task, void (*fn)(void *), void *arg);

#endif /* CGRTOS_ARCH_PORT_H */
