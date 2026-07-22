/**
 * @file safety.c
 * @brief 内核安全监控层：ISR 非法 API、关中断时长、统一异常入口（模块4）
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
static uint64_t g_crit_enter_mtime[CONFIG_NUM_CORES];
static uint64_t g_crit_max_cycles[CONFIG_NUM_CORES];
static uint32_t g_crit_overrun_count;
#ifndef CONFIG_IRQ_DISABLE_WARN_US
#define CONFIG_IRQ_DISABLE_WARN_US  5000U
#endif
#endif

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

uint32_t cgrtos_crit_overrun_count(void)
{
#if CONFIG_IRQ_DISABLE_MONITOR
    return g_crit_overrun_count;
#else
    return 0;
#endif
}

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
 * @brief 阻塞 API 入口：ISR 内禁止
 * @return 0 可继续；非 0 应立即返回 errISR
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

void cgrtos_watchdog_kick(void)
{
#if CONFIG_USE_HOOKS
    if (g_watchdog_hook) {
        g_watchdog_hook();
    }
#endif
}

#if CONFIG_USE_HOOKS
void cgrtos_set_task_create_hook(cgrtos_hook_fn_t hook) { g_task_create_hook = hook; }
void cgrtos_set_task_delete_hook(cgrtos_hook_fn_t hook) { g_task_delete_hook = hook; }
void cgrtos_set_isr_api_hook(cgrtos_hook_fn_t hook) { g_isr_api_hook = hook; }
void cgrtos_set_sched_error_hook(cgrtos_hook_fn_t hook) { g_sched_error_hook = hook; }
void cgrtos_set_irq_exception_hook(cgrtos_hook_fn_t hook) { g_irq_exception_hook = hook; }
void cgrtos_set_watchdog_hook(cgrtos_hook_fn_t hook) { g_watchdog_hook = hook; }
void cgrtos_set_crit_overrun_hook(cgrtos_hook_fn_t hook) { g_crit_overrun_hook = hook; }
#endif
