/**
 * @file timer.c
 * @brief ARM Generic Timer（CNTV）— 导出为 drv_timer_device
 */
#include "../../kernel/cgrtos.h"
#include "hal_board.h"

static uint64_t s_tick_period;

static inline uint64_t cntvct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline void cntv_ctl_write(uint64_t v)
{
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(v));
}

static inline void cntv_tval_write(uint64_t v)
{
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(v));
}

static inline uint64_t cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void timer_schedule_next(void)
{
    if (s_tick_period == 0) {
        s_tick_period = CONFIG_TIMER_CLOCK_HZ / CONFIG_TICK_RATE_HZ;
        if (s_tick_period == 0) {
            s_tick_period = 1;
        }
    }
    cntv_tval_write(s_tick_period);
    cntv_ctl_write(1); /* ENABLE, !IMASK */
}

static uint64_t timer_hw_mtime_read(hal_device_t *dev)
{
    (void)dev;
    return cntvct();
}

static hal_status_t timer_hw_init(hal_device_t *dev, uint32_t tick_hz)
{
    uint64_t frq;
    (void)dev;
    (void)tick_hz;

    frq = cntfrq();
    if (frq == 0) {
        frq = CONFIG_TIMER_CLOCK_HZ;
    }
    s_tick_period = frq / CONFIG_TICK_RATE_HZ;
    if (s_tick_period == 0) {
        s_tick_period = 1;
    }

    cntv_ctl_write(0);
    timer_schedule_next();
    return HAL_OK;
}

static const hal_timer_ops_t s_timer_ops = {
    .init = timer_hw_init,
    .mtime_read = timer_hw_mtime_read,
};

static hal_device_t s_timer_dev = {
    .name = "cntv0",
    .class = HAL_DEV_TIMER,
    .mmio_base = 0,
    .ops = &s_timer_ops,
    .priv = 0,
    .flags = 0,
};

hal_device_t *drv_timer_device(void)
{
    return &s_timer_dev;
}

void arm64_handle_timer(void)
{
    timer_schedule_next();
    cgrtos_tick_handler();
    g_yield_pending[arch_cpu_id()] = 1;
}
