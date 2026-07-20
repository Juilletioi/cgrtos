/**
 * @file plic.c
 * @brief RISC-V PLIC 外部中断控制器驱动。
 *
 * 提供 PLIC 初始化、优先级/使能、claim/complete、阈值，以及
 * 支持高优先级嵌套的快速外部中断分发入口。
 *
 * Nuclei evalsoc DT 按 hart 提供 M+S 两套 context（mei + sei），因此
 * M-mode context 索引为 hart * 2（而非 hart * 1）。
 */

#include "../../kernel/cgrtos.h"

/** @brief PLIC 寄存器基址（Nuclei evalsoc） */
#define PLIC_BASE 0x1C000000
/** @brief 每 context 的 threshold/claim 块跨度 */
#define PLIC_CTX_STRIDE 0x1000
/** @brief 每 context 的 enable 寄存器块跨度 */
#define PLIC_ENABLE_STRIDE 0x80
/**
 * @brief 指定 hart 的 M-mode PLIC context 编号
 * @note evalsoc: contexts 0=hart0-M, 1=hart0-S, 2=hart1-M, 3=hart1-S
 */
#define PLIC_M_CONTEXT(h) ((uint64_t)(h) * 2ULL)
/** @brief 中断源优先级寄存器：base + 4*irq */
#define PLIC_PRIORITY(irq) (PLIC_BASE + 4ULL * (uint64_t)(irq))
/**
 * @brief 指定 hart 的 enable 字地址（每字覆盖 32 个源）
 * @param h    hart 编号
 * @param word 字索引 = irq/32
 */
#define PLIC_ENABLE(h, word) \
    (PLIC_BASE + 0x2000ULL + PLIC_M_CONTEXT(h) * PLIC_ENABLE_STRIDE + \
     4ULL * (uint64_t)(word))
/** @brief 指定 hart 的 PLIC 优先级阈值寄存器（M-mode） */
#define PLIC_THRESHOLD(h) (PLIC_BASE + 0x200000 + PLIC_M_CONTEXT(h) * PLIC_CTX_STRIDE)
/** @brief 指定 hart 的 PLIC claim/complete 寄存器（M-mode，同地址读写） */
#define PLIC_CLAIM(h) (PLIC_BASE + 0x200004 + PLIC_M_CONTEXT(h) * PLIC_CTX_STRIDE)

/**
 * @brief 初始化本 hart 的 PLIC 并使能机器外部中断（MEIE）
 *
 * @details
 * 1. 首次（任意核）调用 cgrtos_irq_init，清零 handler 表与优先级分组上界。
 * 2. 读取 mhartid，将本 hart M-mode context 的优先级阈值寄存器置 0
 *    （允许所有优先级 >0 的外部中断通过）。
 * 3. 通过 set_csr_bits 打开 mie 的 MEIE 位（bit 11）。
 * 4. 每个 hart 在启动路径各自调用一次；irq_init 仅执行一次。
 */
void cgrtos_plic_init(void)
{
    static uint8_t irq_inited;
    /* 1. 全局 IRQ 表初始化（仅一次） */
    if (!irq_inited) {
        cgrtos_irq_init();
        irq_inited = 1;
    }

    /* 2. 本 hart 阈值置 0 */
    uint64_t h = read_csr(mhartid);
    volatile uint32_t *thr = (volatile uint32_t *)PLIC_THRESHOLD(h);
    *thr = 0;
    /* 3. 打开 MEIE */
    set_csr_bits(mie, 0x800);
}

/**
 * @brief 从 PLIC claim 寄存器读取并锁定待服务中断号
 *
 * @return 中断源 ID；0 表示当前无待处理外部中断
 *
 * @details
 * 1. 读取本 hart 编号。
 * 2. 读本 hart M-mode claim 寄存器：PLIC 返回最高优先级 pending 源，并隐式锁定。
 * 3. 返回该 ID；调用方处理完毕后必须 cgrtos_plic_complete，否则该源不再投递。
 */
uint32_t cgrtos_plic_claim(void)
{
    /* 1-2. 读本 hart claim */
    uint64_t h = read_csr(mhartid);
    return *(volatile uint32_t *)PLIC_CLAIM(h);
}

/**
 * @brief 通知 PLIC 指定中断已服务完毕（complete）
 *
 * @param irq 先前 claim 返回的中断源 ID（0 无意义，调用方应避免）
 *
 * @details
 * 1. 读取本 hart 编号。
 * 2. 向 claim/complete 寄存器写入 irq，释放该源锁定，允许再次 pending。
 * 3. 必须与 claim 成对；遗漏会导致该中断源永久丢失后续请求。
 */
void cgrtos_plic_complete(uint32_t irq)
{
    /* 1-2. 写 complete */
    uint64_t h = read_csr(mhartid);
    *(volatile uint32_t *)PLIC_CLAIM(h) = irq;
}

/**
 * @brief 设置本 hart 的 PLIC 优先级阈值
 *
 * @param threshold 新阈值；仅 priority > threshold 的源可投递到本 hart
 *
 * @details
 * 1. 读取 mhartid。
 * 2. 写入本 hart M-mode threshold 寄存器。
 * 3. 用于 enter_critical_from_isr（抬高屏蔽 FromISR 级）与外部 ISR 嵌套窗口。
 */
void cgrtos_plic_set_threshold(uint32_t threshold)
{
    uint64_t h = read_csr(mhartid);
    *(volatile uint32_t *)PLIC_THRESHOLD(h) = threshold;
}

/**
 * @brief 读取本 hart 的 PLIC 优先级阈值
 *
 * @return 当前 threshold 寄存器值
 *
 * @details
 * 1. 读取 mhartid。
 * 2. 返回 threshold 寄存器内容（无副作用）。
 */
uint32_t cgrtos_plic_get_threshold(void)
{
    uint64_t h = read_csr(mhartid);
    return *(volatile uint32_t *)PLIC_THRESHOLD(h);
}

/**
 * @brief 设置中断源优先级
 *
 * @param irq      源编号（1..CONFIG_IRQ_MAX_SOURCES）
 * @param priority 0..CONFIG_IRQ_PRIORITY_MAX；0 表示该源永不投递（PLIC 禁用语义）
 *
 * @return pdPASS 成功；pdFAIL 参数越界
 *
 * @details
 * 1. 校验 irq 与 priority 范围。
 * 2. 写入 PLIC_PRIORITY(irq) 寄存器。
 * 3. 优先级越高越先被 claim；与 threshold 比较决定是否可达本 hart。
 */
int cgrtos_plic_set_priority(uint32_t irq, uint32_t priority)
{
    /* 1. 参数校验 */
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return pdFAIL;
    }
    if (priority > CONFIG_IRQ_PRIORITY_MAX) {
        return pdFAIL;
    }
    /* 2. 写优先级寄存器 */
    *(volatile uint32_t *)PLIC_PRIORITY(irq) = priority;
    return pdPASS;
}

/**
 * @brief 读取中断源优先级
 *
 * @param irq 源编号
 *
 * @return 当前优先级；irq 非法时返回 0
 *
 * @details
 * 1. irq 越界返回 0。
 * 2. 否则读 PLIC_PRIORITY(irq)。
 */
uint32_t cgrtos_plic_get_priority(uint32_t irq)
{
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return 0;
    }
    return *(volatile uint32_t *)PLIC_PRIORITY(irq);
}

/**
 * @brief 对本 hart 使能指定中断源
 *
 * @param irq 源编号（1..CONFIG_IRQ_MAX_SOURCES）
 *
 * @return pdPASS 成功；pdFAIL irq 非法
 *
 * @details
 * 1. 校验 irq。
 * 2. 计算 enable 字索引 word=irq/32 与位 bit=irq%32。
 * 3. 对本 hart M-mode enable 字做按位置 1。
 * 4. 仅影响本 hart；其他 hart 需各自使能。
 */
int cgrtos_plic_enable(uint32_t irq)
{
    /* 1. 参数校验 */
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return pdFAIL;
    }
    /* 2-3. 置 enable 位 */
    uint64_t h = read_csr(mhartid);
    uint32_t word = irq / 32U;
    uint32_t bit = irq % 32U;
    volatile uint32_t *en = (volatile uint32_t *)PLIC_ENABLE(h, word);
    *en |= (1U << bit);
    return pdPASS;
}

/**
 * @brief 对本 hart 禁用指定中断源
 *
 * @param irq 源编号
 *
 * @return pdPASS 成功；pdFAIL irq 非法
 *
 * @details
 * 1. 校验 irq。
 * 2. 计算 word/bit，对本 hart enable 字按位清 0。
 * 3. 已 pending 的请求在 disable 后通常不再投递到本 hart。
 */
int cgrtos_plic_disable(uint32_t irq)
{
    /* 1. 参数校验 */
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return pdFAIL;
    }
    /* 2. 清 enable 位 */
    uint64_t h = read_csr(mhartid);
    uint32_t word = irq / 32U;
    uint32_t bit = irq % 32U;
    volatile uint32_t *en = (volatile uint32_t *)PLIC_ENABLE(h, word);
    *en &= ~(1U << bit);
    return pdPASS;
}

/**
 * @brief 机器外部中断 C 层入口（快速响应路径）
 *
 * @param f 陷阱栈帧指针（本实现未使用，由 trap_vector 传入）
 *
 * @details
 * 1. cgrtos_isr_enter 标记 ISR 上下文（供 cgrtos_in_isr 查询）。
 * 2. claim 中断号；若为 0 则跳到步骤 3。
 *    a. 保存当前 threshold；将 threshold 抬至该源优先级（prio==0 时抬到 1），
 *       屏蔽同级及以下，为更高优先级嵌套做准备。
 *    b. 若 CONFIG_IRQ_NESTING：保存 mie，清 MTIE|MSIE，置 mstatus.MIE，
 *       仅允许更高优先级 *外部* 中断嵌套（避免 SysTimer/IPI 重入）。
 *    c. cgrtos_irq_dispatch(irq) 调用注册 handler（无则空操作）。
 *    d. 若开启嵌套：清 MIE，恢复 mie；恢复 threshold；complete(irq)。
 * 3. cgrtos_isr_exit。
 *
 * @note 更高优先级嵌套 ISR 不得调用 FromISR（优先级应 > syscall_max）。
 */
void riscv_handle_external(uint64_t *f)
{
    (void)f;
    /* 1. 进入 ISR 上下文 */
    cgrtos_isr_enter();

    /* 2. claim */
    uint32_t irq = cgrtos_plic_claim();
    if (irq) {
        uint32_t saved_thr = cgrtos_plic_get_threshold();
        uint32_t prio = cgrtos_plic_get_priority(irq);
        /* a. 防御：prio==0 时至少抬到 1，避免 threshold=0 放行全部 */
        if (prio == 0) {
            prio = 1;
        }
        cgrtos_plic_set_threshold(prio);

#if CONFIG_IRQ_NESTING
        /* b. 仅嵌套更高优先级外部中断 */
        uint64_t mie_save;
        asm volatile("csrr %0, mie" : "=r"(mie_save));
        asm volatile("csrc mie, %0" :: "r"(0x88ULL));
        set_csr_bits(mstatus, 0x8);
#endif
        /* c. 分发 */
        cgrtos_irq_dispatch(irq);
#if CONFIG_IRQ_NESTING
        clear_csr_bits(mstatus, 0x8);
        asm volatile("csrw mie, %0" :: "r"(mie_save));
#endif

        /* d. 恢复 threshold 并 complete */
        cgrtos_plic_set_threshold(saved_thr);
        cgrtos_plic_complete(irq);
    }

    /* 3. 退出 ISR 上下文 */
    cgrtos_isr_exit();
}

/**
 * @brief 同步异常 C 层入口（非法指令、访存错误等）
 *
 * @param f     陷阱栈帧（本实现未使用）
 * @param cause mcause 异常原因编码（不含中断位）
 * @param epc   mepc：异常发生时的 PC
 *
 * @details
 * 1. 接收 trap_vector 传入的 cause 与 epc。
 * 2. 通过 cgrtos_printf 打印异常信息供调试。
 * 3. 当前不恢复、不 halt；返回后行为取决于 trap 路径（通常继续执行或再次异常）。
 */
void riscv_handle_exception(uint64_t *f, uint64_t cause, uint64_t epc)
{
    (void)f;
    /* 1-2. 打印诊断信息 */
    cgrtos_printf("[EXC] cause=%lu epc=%p\n",
                  (unsigned long)cause, (void *)(uintptr_t)epc);
}
