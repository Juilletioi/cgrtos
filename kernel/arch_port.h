/**
 * @file arch_port.h
 * @brief CPU 移植层：核 ID、关中断、自愿让出、idle、IPI 使能、任务栈帧
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
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
 * @details 读取架构亲和寄存器低 8 位：RISC-V 为 mhartid，AArch64 为 MPIDR_EL1 Aff0。
 * @return 核号（unsigned）
 * @retval 0..(CONFIG_NUM_CORES-1) 有效逻辑核
 * @note 与 g_current[] / ready 队列下标一致；无锁、无内存屏障。
 * @warning 超出 CONFIG_NUM_CORES 的物理 ID 须由板级保证不会出现。
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
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
 * @details AArch64：读 DAIF 后 daifset #2；RISC-V：读 mstatus 后清除 MIE。
 * @return 架构相关 flags（mstatus 或 DAIF 快照）
 * @retval 非特定值 须原样传给 arch_irq_restore
 * @note 与 arch_irq_restore 成对；可嵌套时须各自保存/恢复。
 * @warning 长时间关中断会抬高延迟；勿在持锁路径外泄漏 flags。
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
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
 * @details 将 flags 写回 DAIF 或 mstatus，恢复调用前的 IRQ 使能态。
 * @param[in] flags arch_irq_save 返回值
 * @return 无
 * @retval 无
 * @note 必须与同一次 arch_irq_save 配对；顺序错误会导致 IRQ 永久屏蔽或提前打开。
 * @warning 传入非本核/非配对 flags 行为未定义。
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
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
 * @return 无
 * @retval 无
 * @note AArch64：ELR 已指向下一条指令，SVC 处理勿再加 4；RISC-V 用 ecall。
 * @warning 禁止在 ISR 中调用；持自旋锁时调用可能导致死锁。
 * @attention ❌ 不允许在中断上下文调用；✅ 可能阻塞/引起调度（上下文切换）
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
 * @details 执行 WFI；有待处理中断或事件时醒来继续执行。
 * @return 无
 * @retval 无
 * @note idle 任务与致命挂起路径使用；非睡眠 API（无超时）。
 * @warning QEMU 上若定时器依赖忙等，可能需配合 CONFIG_IDLE_BUSY_PUMP。
 * @attention ✅ 允许在中断上下文调用；❌ 不会按 sleep 语义阻塞（WFI 可被中断唤醒，不引起任务调度请求）
 */
static inline void arch_cpu_wait(void)
{
    __asm__ volatile("wfi" ::: "memory");
}

/**
 * @brief 使能本核核间中断接收（次核启动路径）
 * @details RISC-V 置 mie.MSIE；AArch64 当前为 no-op（无 SGI IPI）。
 * @return 无
 * @retval 无
 * @note 通常由次核启动在进入调度前调用一次。
 * @warning 重复调用不改变语义；AArch64 在补齐 SGI 前无实际效果。
 * @attention ❌ 不允许在中断上下文调用（启动路径）；❌ 不会阻塞/引起调度
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
 * @details 按架构约定布置初始寄存器与返回地址，使首次调度从 fn(arg) 进入。
 * @param[in,out] task TCB；须已分配栈区
 * @param[in] fn 入口函数
 * @param[in] arg 参数（a0 / x0）
 * @return 无
 * @retval 无
 * @note 由任务创建路径调用；用户勿直接改 sp 帧布局。
 * @warning task/fn 为 NULL 时行为未定义。
 * @attention ❌ 不允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
void arch_task_stack_init(struct cgrtos_task *task, void (*fn)(void *), void *arg);

#endif /* CGRTOS_ARCH_PORT_H */
