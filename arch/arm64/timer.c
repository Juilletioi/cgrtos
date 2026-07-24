/**
 * @file timer.c
 * @brief ARM Generic Timer（CNTV）— 导出为 drv_timer_device
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * - 使用 Virtual timer（CNTV_CTL / CNTV_TVAL / CNTVCT）。
 * - ops 供 HAL；arm64_handle_timer 由 GIC IRQ 路径直调，不经 hal_timer_*。
 * - tick 周期 = CNTFRQ（或 CONFIG_TIMER_CLOCK_HZ）/ CONFIG_TICK_RATE_HZ。
 */

#include "../../kernel/cgrtos.h"
#include "hal_board.h"

static uint64_t s_tick_period;

/**
 * @brief 读 CNTVCT_EL0 虚拟计数器
 * @details
 * 1. MRS 读 cntvct_el0。
 * 2. 返回 64 位计数值。
 * @return 当前虚拟计数
 * @retval >=0 有效计数
 * @note 只读系统寄存器，可无锁调用
 * @warning 无
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline uint64_t cntvct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/**
 * @brief 写 CNTV_CTL_EL0 控制寄存器
 * @details
 * 1. MSR 写 cntv_ctl_el0 = v。
 * 2. bit0=ENABLE，bit1=IMASK（本驱动用 1 表示 ENABLE 且未屏蔽）。
 * @param[in] v 控制字
 * @return 无
 * @retval 无
 * @note 供 schedule_next / init 使用
 * @warning 写 0 会停止 Virtual timer
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline void cntv_ctl_write(uint64_t v)
{
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(v));
}

/**
 * @brief 写 CNTV_TVAL_EL0 相对超时值
 * @details
 * 1. MSR 写 cntv_tval_el0 = v。
 * 2. 硬件以当前 CNTVCT 为基准预约下一次比较。
 * @param[in] v 相对 tick 周期（计数器单位）
 * @return 无
 * @retval 无
 * @note 配合 CTL.ENABLE 触发 PPI
 * @warning v=0 可能导致立即再入
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline void cntv_tval_write(uint64_t v)
{
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(v));
}

/**
 * @brief 读 CNTFRQ_EL0 计数器频率
 * @details
 * 1. MRS 读 cntfrq_el0。
 * 2. 返回固件/仿真器提供的频率；QEMU 可能为 0。
 * @return 频率 Hz
 * @retval >0 有效频率
 * @retval 0   未提供（调用方回退 CONFIG_TIMER_CLOCK_HZ）
 * @note 仅 init 使用
 * @warning 无
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline uint64_t cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/**
 * @brief 按 s_tick_period 预约下一次 Virtual timer 中断
 * @details
 * 1. 若 s_tick_period 为 0，按 CONFIG_TIMER_CLOCK_HZ / CONFIG_TICK_RATE_HZ 计算（至少 1）。
 * 2. cntv_tval_write(s_tick_period)。
 * 3. cntv_ctl_write(1) 使能且不屏蔽。
 * @return 无
 * @retval 无
 * @note 由 init 与 arm64_handle_timer 调用
 * @warning 依赖 CONFIG_* 与 CNTFRQ 一致，否则 tick 漂移
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
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

/**
 * @brief 读虚拟时间基（对齐 HAL mtime_read）
 * @details
 * 1. 忽略 dev。
 * 2. 返回 cntvct()。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return 当前 CNTVCT
 * @retval >=0 有效计数
 * @note 由 HAL hal_mtime_read 经 ops-->mtime_read 调用
 * @warning 无
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static uint64_t timer_hw_mtime_read(hal_device_t *dev)
{
    (void)dev;
    return cntvct();
}

/**
 * @brief Virtual timer 硬件初始化
 * @details
 * 1. 读 CNTFRQ；为 0 则用 CONFIG_TIMER_CLOCK_HZ。
 * 2. 计算 s_tick_period = frq / CONFIG_TICK_RATE_HZ（至少 1）。
 * 3. 先 CTL=0 关闭，再 timer_schedule_next 预约首次 tick。
 * @param[in] dev     设备描述符；本驱动未使用
 * @param[in] tick_hz 期望频率；本实现以 CONFIG_TICK_RATE_HZ 为准
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_timer_init 经 ops-->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
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

/**
 * @brief 向 HAL 导出 CNTV 定时器设备描述符
 * @details
 * 1. 返回静态 s_timer_dev 指针。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 勿释放返回值；应用应使用 hal_timer_init / hal_mtime_read
 * @warning 驱动不得自行调用 hal_device_register
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 */
hal_device_t *drv_timer_device(void)
{
    return &s_timer_dev;
}

/**
 * @brief Virtual timer PPI 中断 C 入口（由 GIC 路径直调）
 * @details
 * 1. timer_schedule_next 重载下一次比较。
 * 2. cgrtos_tick_handler 推进内核 tick。
 * 3. 置本 CPU g_yield_pending=1 请求调度。
 * @return 无
 * @retval 无
 * @note 已在 arm64_handle_irq 的 isr_enter/exit 包裹内；禁止再绕回 hal_timer_*
 * @warning 须在 IRQ 上下文调用
 * @attention ✅ ISR-safe；✅ 可能引起上下文切换（tick + g_yield_pending）
 */
void arm64_handle_timer(void)
{
    timer_schedule_next();
    cgrtos_tick_handler();
    g_yield_pending[arch_cpu_id()] = 1;
}
