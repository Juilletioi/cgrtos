/**
 * @file irq.c
 * @brief 中断优先级分组、PLIC 处理注册、ISR 临界区与 woken 通知。
 *
 * @details
 * 优先级分组（FreeRTOS 风格）：
 *   - 优先级 ≤ syscall_max_prio 的中断可调用 FromISR；
 *   - enter_critical_from_isr 将 PLIC threshold 抬至 syscall_max_prio，
 *     屏蔽这些中断，同时允许更高优先级中断嵌套（不得调用 FromISR）。
 * 快速响应：riscv_handle_external 在分发前把 threshold 设为当前源优先级；
 * 当 CONFIG_IRQ_NESTING=1 时短暂打开 MIE（并关 MTIE/MSIE），使更高优先级
 * 外部中断可嵌套抢占。
 */
#include "cgrtos.h"

/**
 * @brief 已注册的 handler 表
 * @note 下标 = 中断源号；下标 0 保留不用（PLIC 源从 1 起）
 */
static cgrtos_irq_handler_t g_irq_handlers[CONFIG_IRQ_MAX_SOURCES + 1];
/**
 * @brief 与 g_irq_handlers 一一对应的私有参数，在 dispatch 时原样传给 handler
 */
static void                *g_irq_args[CONFIG_IRQ_MAX_SOURCES + 1];
/**
 * @brief 运行时 FromISR 允许的最高优先级
 * @note 默认等于 CONFIG_IRQ_SYSCALL_MAX_PRIO；可由 set_syscall_max_priority 修改
 */
static uint32_t             g_syscall_max_prio = CONFIG_IRQ_SYSCALL_MAX_PRIO;

/**
 * @brief 初始化 IRQ 子系统
 *
 * @details
 * 1. 将 g_irq_handlers[] 与 g_irq_args[] 全部清零（无已注册 handler）。
 * 2. 将 g_syscall_max_prio 复位为 CONFIG_IRQ_SYSCALL_MAX_PRIO。
 * 3. 由 cgrtos_plic_init 在首次调用时触发；可重复调用（幂等清表）。
 */
void cgrtos_irq_init(void)
{
    /* 1. 清零 handler 与 arg 表 */
    for (uint32_t i = 0; i <= CONFIG_IRQ_MAX_SOURCES; i++) {
        g_irq_handlers[i] = 0;
        g_irq_args[i] = 0;
    }
    /* 2. 复位优先级分组上界 */
    g_syscall_max_prio = CONFIG_IRQ_SYSCALL_MAX_PRIO;
}

/**
 * @brief 配置中断源优先级并可选使能/禁用
 *
 * @param irq      中断源编号，范围 1..CONFIG_IRQ_MAX_SOURCES
 * @param priority 优先级 0..CONFIG_IRQ_PRIORITY_MAX；0 表示禁用该源（PLIC 语义）
 * @param enable   非 0 则对本 hart 使能该源；0 则禁用
 *
 * @return pdPASS 成功；pdFAIL 参数非法或底层 PLIC 写入失败
 *
 * @details
 * 1. 校验 irq 与 priority 合法范围。
 * 2. 调用 cgrtos_plic_set_priority 写入源优先级寄存器。
 * 3. 按 enable 调用 plic_enable 或 plic_disable。
 * 4. 本函数不注册 handler；需另调 cgrtos_irq_register。
 */
int cgrtos_irq_configure(uint32_t irq, uint32_t priority, int enable)
{
    /* 1. 参数范围校验 */
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return pdFAIL;
    }
    if (priority > CONFIG_IRQ_PRIORITY_MAX) {
        return pdFAIL;
    }
    /* 2. 写 PLIC 优先级 */
    if (cgrtos_plic_set_priority(irq, priority) != pdPASS) {
        return pdFAIL;
    }
    /* 3. 使能或禁用 */
    if (enable) {
        return cgrtos_plic_enable(irq);
    }
    return cgrtos_plic_disable(irq);
}

/**
 * @brief 注册 PLIC 外部中断处理函数
 *
 * @param irq     中断源编号（1..CONFIG_IRQ_MAX_SOURCES）
 * @param handler 回调；签名 void (*)(uint32_t irq, void *arg)
 * @param arg     透传给 handler 的私有指针（可为 NULL）
 *
 * @return pdPASS 成功；pdFAIL 参数非法（irq 越界或 handler 为空）
 *
 * @details
 * 1. 校验 irq 与 handler。
 * 2. 将 handler/arg 写入表项 g_irq_handlers[irq] / g_irq_args[irq]。
 * 3. 实际调用发生在 riscv_handle_external → cgrtos_irq_dispatch。
 * 4. handler 内可调用 FromISR（仅当该源优先级 ≤ syscall_max）；不得阻塞。
 */
int cgrtos_irq_register(uint32_t irq, cgrtos_irq_handler_t handler, void *arg)
{
    /* 1. 参数校验 */
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES || !handler) {
        return pdFAIL;
    }
    /* 2. 写入注册表 */
    g_irq_handlers[irq] = handler;
    g_irq_args[irq] = arg;
    return pdPASS;
}

/**
 * @brief 注销指定中断源的处理函数
 *
 * @param irq 中断源编号
 *
 * @return pdPASS 成功；pdFAIL irq 非法
 *
 * @details
 * 1. 校验 irq 范围。
 * 2. 将 g_irq_handlers[irq] 与 g_irq_args[irq] 清零。
 * 3. 不自动 disable PLIC 源；若需屏蔽请另调 plic_disable / irq_configure。
 */
int cgrtos_irq_unregister(uint32_t irq)
{
    /* 1. 参数校验 */
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return pdFAIL;
    }
    /* 2. 清表项 */
    g_irq_handlers[irq] = 0;
    g_irq_args[irq] = 0;
    return pdPASS;
}

/**
 * @brief 查询已注册的 handler（诊断/测试用）
 *
 * @param irq 中断源编号
 *
 * @return 已注册函数指针；未注册或 irq 非法时返回 NULL
 *
 * @details
 * 1. irq 越界返回 NULL。
 * 2. 否则返回 g_irq_handlers[irq]（可能为 NULL 表示未注册）。
 */
cgrtos_irq_handler_t cgrtos_irq_get_handler(uint32_t irq)
{
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return 0;
    }
    return g_irq_handlers[irq];
}

/**
 * @brief 设置允许调用 FromISR 的最高中断优先级（优先级分组上界）
 *
 * @param max_prio 新上界；超过 CONFIG_IRQ_PRIORITY_MAX 时钳制到该最大值
 *
 * @details
 * 1. 将 max_prio 钳制到 ≤ CONFIG_IRQ_PRIORITY_MAX。
 * 2. 写入 g_syscall_max_prio。
 * 3. enter_critical_from_isr 使用该值作为 PLIC threshold，从而：
 *    - 屏蔽优先级 ≤ max_prio 的中断（这些中断才允许调 FromISR）；
 *    - 更高优先级仍可嵌套，但约定不得调用 FromISR。
 */
void cgrtos_irq_set_syscall_max_priority(uint32_t max_prio)
{
    /* 1. 钳制上限 */
    if (max_prio > CONFIG_IRQ_PRIORITY_MAX) {
        max_prio = CONFIG_IRQ_PRIORITY_MAX;
    }
    /* 2. 更新运行时分组上界 */
    g_syscall_max_prio = max_prio;
}

/**
 * @brief 读取当前 FromISR 优先级上界
 *
 * @return g_syscall_max_prio 当前值（默认 CONFIG_IRQ_SYSCALL_MAX_PRIO）
 *
 * @details
 * 1. 直接返回静态变量 g_syscall_max_prio，无副作用。
 */
uint32_t cgrtos_irq_get_syscall_max_priority(void)
{
    return g_syscall_max_prio;
}

/**
 * @brief 将已 claim 的外部中断分发到注册 handler
 *
 * @param irq PLIC claim 返回的中断源号
 *
 * @details
 * 1. irq 为 0 或越界则直接返回（防御性）。
 * 2. 查表 g_irq_handlers[irq]；若非空则调用 h(irq, g_irq_args[irq])。
 * 3. 无 handler 时为空操作，调用方仍须 complete，避免 PLIC 卡死。
 * 4. 由 riscv_handle_external 在嵌套窗口内调用。
 */
void cgrtos_irq_dispatch(uint32_t irq)
{
    /* 1. 范围检查 */
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return;
    }
    /* 2-3. 有 handler 则调用，否则空操作 */
    cgrtos_irq_handler_t h = g_irq_handlers[irq];
    if (h) {
        h(irq, g_irq_args[irq]);
    }
}

/**
 * @brief ISR 内临界区：抬高 PLIC threshold 屏蔽 FromISR 级中断
 *
 * @return 进入前的 threshold，必须交给 cgrtos_exit_critical_from_isr 成对恢复
 *
 * @details
 * 1. 读取本 hart 当前 PLIC threshold 保存为 prev。
 * 2. 将 threshold 设为 g_syscall_max_prio：
 *    PLIC 仅投递 priority > threshold 的源，故 ≤ syscall_max 的中断被屏蔽。
 * 3. 更高优先级中断仍可到达（若 CONFIG_IRQ_NESTING 且 MIE 打开），但不得调 FromISR。
 * 4. 与任务侧 cgrtos_enter_critical（关 MIE + g_klock）不同：本 API 只改 threshold。
 */
uint32_t cgrtos_enter_critical_from_isr(void)
{
    /* 1. 保存旧阈值 */
    uint32_t prev = cgrtos_plic_get_threshold();
    /* 2. 抬高至 syscall_max，屏蔽 FromISR 级源 */
    cgrtos_plic_set_threshold(g_syscall_max_prio);
    return prev;
}

/**
 * @brief 退出 ISR 临界区，恢复 PLIC threshold
 *
 * @param saved_threshold 先前 enter_critical_from_isr 的返回值
 *
 * @details
 * 1. 将本 hart PLIC threshold 写回 saved_threshold。
 * 2. 必须与 enter 成对；嵌套调用时由调用方自行维护保存栈。
 */
void cgrtos_exit_critical_from_isr(uint32_t saved_threshold)
{
    cgrtos_plic_set_threshold(saved_threshold);
}

/**
 * @brief FromISR 统一唤醒通知（woken 或自动 yield）
 *
 * @param woken      可选输出指针；非空且 need_yield 时置 *woken = pdTRUE，
 *                   由 ISR 末尾 portYIELD_FROM_ISR(woken) 请求调度
 * @param need_yield 非 0 表示本 FromISR 唤醒了可能更高优先级的任务
 *
 * @details
 * 1. need_yield==0 时立即返回（无调度请求）。
 * 2. woken 非空：仅置 *woken=pdTRUE，不直接 yield（兼容 FreeRTOS 批量 FromISR）。
 * 3. woken 为空：调用 cgrtos_sched_yield_from_isr，在 trap 返回路径切换。
 */
void cgrtos_isr_notify_woken(BaseType_t *woken, int need_yield)
{
    /* 1. 无需调度 */
    if (!need_yield) {
        return;
    }
    /* 2. 延迟 yield：交给调用方 portYIELD_FROM_ISR */
    if (woken) {
        *woken = pdTRUE;
    } else {
        /* 3. 立即请求 trap 出口调度 */
        cgrtos_sched_yield_from_isr();
    }
}
