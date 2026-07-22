/**
 * @file safety.c
 * @brief 内核安全监控层：ISR 非法 API、关中断时长、统一异常入口（模块 4）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 */

#include "cgrtos.h"

#if CONFIG_USE_HOOKS
cgrtos_hook_fn_t g_task_create_hook;
cgrtos_hook_fn_t g_task_delete_hook;
cgrtos_hook_fn_t g_isr_api_hook;
cgrtos_hook_fn_t g_sched_error_hook;
cgrtos_hook_fn_t g_irq_exception_hook;
cgrtos_hook_fn_t g_watchdog_hook;
cgrtos_hook_fn_t g_crit_overrun_hook;
#endif

#if CONFIG_IRQ_DISABLE_MONITOR
/** @brief 各核进入临界区时的 mtime 快照 */
static uint64_t g_crit_enter_mtime[CONFIG_NUM_CORES];
/** @brief 各核观测到的最长关中断周期（mtime 差） */
static uint64_t g_crit_max_cycles[CONFIG_NUM_CORES];
/** @brief 关中断超时累计次数 */
static uint32_t g_crit_overrun_count;
#ifndef CONFIG_IRQ_DISABLE_WARN_US
#define CONFIG_IRQ_DISABLE_WARN_US  5000U
#endif
#endif

/**
 * @brief 临界区进入钩子（记录关中断起始时刻）
 * @details 在 cgrtos_enter_critical 路径调用，记录当前核 mtime。纯时间戳写入，不阻塞、不切换。
 * @param[in] cpu 逻辑 CPU 编号
 * @return 无
 * @retval 无
 * @note CONFIG_IRQ_DISABLE_MONITOR 关闭时为 no-op
 * @warning 须与 cgrtos_safety_on_crit_exit 成对调用
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_safety_on_crit_enter(uint8_t cpu)
{
#if CONFIG_IRQ_DISABLE_MONITOR
    if (cpu < CONFIG_NUM_CORES) {
        g_crit_enter_mtime[cpu] = cgrtos_mtime_read();
    }
#else
    (void)cpu;
#endif
}

/**
 * @brief 临界区退出钩子（检测关中断时长）
 * @details 计算 mtime 差值，更新最大关中断周期；超 CONFIG_IRQ_DISABLE_WARN_US 则计数并可选触发 crit_overrun_hook。不切换任务。
 * @param[in] cpu 逻辑 CPU 编号
 * @return 无
 * @retval 无
 * @note 超时仅告警，不自动恢复调度
 * @warning 过长关中断会延迟中断响应
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_safety_on_crit_exit(uint8_t cpu)
{
#if CONFIG_IRQ_DISABLE_MONITOR
    uint64_t now, dt, limit;
    if (cpu >= CONFIG_NUM_CORES) {
        return;
    }
    now = cgrtos_mtime_read();
    dt = now - g_crit_enter_mtime[cpu];
    if (dt > g_crit_max_cycles[cpu]) {
        g_crit_max_cycles[cpu] = dt;
    }
    /* mtime @ CONFIG_TIMER_CLOCK_HZ */
    limit = ((uint64_t)CONFIG_IRQ_DISABLE_WARN_US *
             (CONFIG_TIMER_CLOCK_HZ / 1000000ULL));
    if (limit == 0) {
        limit = (CONFIG_TIMER_CLOCK_HZ / 200ULL); /* ~5ms default-ish */
    }
    if (dt > limit) {
        g_crit_overrun_count++;
#if CONFIG_USE_HOOKS
        if (g_crit_overrun_hook) {
            g_crit_overrun_hook();
        }
#endif
        CGRTOS_LOGW("crit", "long IRQ-off detected");
    }
#else
    (void)cpu;
#endif
}

/**
 * @brief 查询关中断超时累计次数
 * @details 返回 g_crit_overrun_count。只读，不阻塞、不切换。
 * @return 超时事件计数
 * @retval >=0 累计次数（MONITOR 关闭时恒为 0）
 * @note 每次 cgrtos_safety_on_crit_exit 检测到超时递增 1
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
uint32_t cgrtos_crit_overrun_count(void)
{
#if CONFIG_IRQ_DISABLE_MONITOR
    return g_crit_overrun_count;
#else
    return 0;
#endif
}

/**
 * @brief 查询指定核历史最长关中断周期
 * @details 返回 g_crit_max_cycles[cpu]（mtime  tick 数）。只读，不阻塞、不切换。
 * @param[in] cpu 逻辑 CPU 编号
 * @return 最长关中断 mtime 差；cpu 越界或 MONITOR 关闭时返回 0
 * @retval >0    观测到的最大周期
 * @retval 0     cpu 无效或功能未启用
 * @note 可用于诊断临界区持有时间
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
uint64_t cgrtos_crit_max_cycles(uint8_t cpu)
{
#if CONFIG_IRQ_DISABLE_MONITOR
    if (cpu >= CONFIG_NUM_CORES) {
        return 0;
    }
    return g_crit_max_cycles[cpu];
#else
    (void)cpu;
    return 0;
#endif
}

/**
 * @brief 阻塞 API 入口守卫：ISR 内禁止
 * @details 若当前处于 ISR 则记录错误、可选触发 isr_api_hook 并返回 1；任务上下文返回 0 允许继续。不切换任务。
 * @return 0 可继续；非 0 应立即返回 errISR
 * @retval 0 非 ISR 上下文，允许调用阻塞 API
 * @retval 1 处于 ISR，调用方须中止并返回 errISR
 * @note 由各阻塞型内核 API 在入口处调用
 * @warning 忽略返回值将导致 ISR 中非法阻塞
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_reject_blocking_in_isr(void)
{
    if (!cgrtos_in_isr()) {
        return 0;
    }
#if CONFIG_USE_HOOKS
    if (g_isr_api_hook) {
        g_isr_api_hook();
    }
#endif
    CGRTOS_LOGE("api", "blocking API called from ISR");
    return 1;
}

/**
 * @brief 统一致命错误入口
 * @details 可选触发 irq_exception_hook，打印 [FATAL] 信息后调用 cgrtos_assert_failed 停止系统。不返回。
 * @param[in] reason 错误描述；NULL 时使用 "?"
 * @param[in] code   错误码
 * @return 无（不返回）
 * @retval 无
 * @note 调用后系统进入 assert 处理路径
 * @warning 不可从期望恢复的路径调用
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_fatal_error(const char *reason, int code)
{
#if CONFIG_USE_HOOKS
    if (g_irq_exception_hook) {
        g_irq_exception_hook();
    }
#endif
    cgrtos_printf("[FATAL] %s code=%d\n", reason ? reason : "?", code);
    cgrtos_assert_failed(reason ? reason : "fatal", code);
}

/**
 * @brief 看门狗喂狗钩子
 * @details 若已注册 watchdog_hook 则调用。纯回调转发，不阻塞、不切换。
 * @return 无
 * @retval 无
 * @note CONFIG_USE_HOOKS 关闭时为 no-op
 * @warning 钩子内不宜执行耗时操作
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_watchdog_kick(void)
{
#if CONFIG_USE_HOOKS
    if (g_watchdog_hook) {
        g_watchdog_hook();
    }
#endif
}

#if CONFIG_USE_HOOKS

/**
 * @brief 注册任务创建钩子
 * @details 设置 g_task_create_hook。不阻塞、不切换。
 * @param[in] hook 回调函数；NULL 清除钩子
 * @return 无
 * @retval 无
 * @note 任务创建成功路径调用
 * @warning 钩子内不可调用阻塞 API
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_set_task_create_hook(cgrtos_hook_fn_t hook) { g_task_create_hook = hook; }

/**
 * @brief 注册任务删除钩子
 * @details 设置 g_task_delete_hook。不阻塞、不切换。
 * @param[in] hook 回调函数；NULL 清除钩子
 * @return 无
 * @retval 无
 * @note 任务删除路径调用
 * @warning 钩子内不可调用阻塞 API
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_set_task_delete_hook(cgrtos_hook_fn_t hook) { g_task_delete_hook = hook; }

/**
 * @brief 注册 ISR 非法 API 钩子
 * @details 设置 g_isr_api_hook，在 cgrtos_reject_blocking_in_isr 检测到违规时调用。
 * @param[in] hook 回调函数；NULL 清除钩子
 * @return 无
 * @retval 无
 * @note 无
 * @warning 钩子须极短，运行于 ISR 上下文
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_set_isr_api_hook(cgrtos_hook_fn_t hook) { g_isr_api_hook = hook; }

/**
 * @brief 注册调度错误钩子
 * @details 设置 g_sched_error_hook。不阻塞、不切换。
 * @param[in] hook 回调函数；NULL 清除钩子
 * @return 无
 * @retval 无
 * @note 调度器内部错误路径调用
 * @warning 钩子内不可触发任务切换
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_set_sched_error_hook(cgrtos_hook_fn_t hook) { g_sched_error_hook = hook; }

/**
 * @brief 注册 IRQ 异常钩子
 * @details 设置 g_irq_exception_hook，在 cgrtos_fatal_error 前调用。
 * @param[in] hook 回调函数；NULL 清除钩子
 * @return 无
 * @retval 无
 * @note 无
 * @warning 钩子应快速返回或记录状态
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_set_irq_exception_hook(cgrtos_hook_fn_t hook) { g_irq_exception_hook = hook; }

/**
 * @brief 注册看门狗钩子
 * @details 设置 g_watchdog_hook，由 cgrtos_watchdog_kick 调用。
 * @param[in] hook 回调函数；NULL 清除钩子
 * @return 无
 * @retval 无
 * @note 无
 * @warning 钩子内不宜阻塞
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_set_watchdog_hook(cgrtos_hook_fn_t hook) { g_watchdog_hook = hook; }

/**
 * @brief 注册关中断超时钩子
 * @details 设置 g_crit_overrun_hook，在检测到过长关中断时调用。
 * @param[in] hook 回调函数；NULL 清除钩子
 * @return 无
 * @retval 无
 * @note 无
 * @warning 钩子运行于 crit_exit 路径，须保持简短
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_set_crit_overrun_hook(cgrtos_hook_fn_t hook) { g_crit_overrun_hook = hook; }

#endif
