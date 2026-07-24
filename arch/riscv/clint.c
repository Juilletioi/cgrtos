/**
 * @file clint.c
 * @brief Nuclei SysTimer / CLINT 定时器驱动（纯硬件层）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * - 提供 timer ops；trap 入口 riscv_handle_timer 直接调本文件静态例程。
 * - 禁止调用 hal_*；用户读 mtime 请用 hal_mtime_read()。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

#ifndef CONFIG_TIMER_CLOCK_HZ
/** @brief 缺省定时器时钟（Hz）；板级未配置时回退 1 MHz */
#define CONFIG_TIMER_CLOCK_HZ  1000000ULL
#endif

extern void cgrtos_isr_enter(void);
extern void cgrtos_isr_exit(void);

/**
 * @brief 读 mtime 自由运行计数器
 * @details
 * 1. volatile 读 HAL_BOARD_MTIME_ADDR。
 * 2. 返回 64 位计数值。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return 当前 mtime 计数值
 * @retval >=0 有效计数
 * @note 只读 MMIO，可在任意上下文无锁调用
 * @warning 无
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static uint64_t clint_hw_mtime_read(hal_device_t *dev)
{
    (void)dev;
    return *(volatile uint64_t *)HAL_BOARD_MTIME_ADDR;
}

/**
 * @brief 写本 hart 的 mtimecmp 并执行内存屏障
 * @details
 * 1. 按 hartid 计算 MTIMECMP 寄存器地址。
 * 2. 写入比较值 val。
 * 3. fence iorw 保证对后续 mtime 读可见。
 * @param[in] hartid 目标 hart 编号
 * @param[in] val    新的 mtimecmp 比较值
 * @return 无
 * @retval 无
 * @note 内联函数，供 clint_schedule_next 与 ISR 路径调用
 * @warning hartid 须合法且对应已映射的 MTIMECMP
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline void mtimecmp_write(uint64_t hartid, uint64_t val)
{
    *(volatile uint64_t *)HAL_BOARD_MTIMECMP_ADDR(hartid) = val;
    asm volatile("fence iorw, iorw" ::: "memory");
}

/**
 * @brief 按 CONFIG 频率预约下一次 MTIP 中断
 * @details
 * 1. tpi = TIMER_CLOCK_HZ / TICK_RATE_HZ（至少 1）。
 * 2. now = mtime；写入 mtimecmp = now + tpi。
 * 3. 若写后 mtime 已越过比较值，基于最新 mtime 重写，避免丢中断。
 * @param[in] hartid 本 hart 编号
 * @return 无
 * @retval 无
 * @note 由 init 与 riscv_handle_timer 调用
 * @warning 依赖 CONFIG_TIMER_CLOCK_HZ 与 CONFIG_TICK_RATE_HZ 正确配置
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static void clint_schedule_next(uint64_t hartid)
{
    uint64_t tpi = CONFIG_TIMER_CLOCK_HZ / CONFIG_TICK_RATE_HZ;
    if (tpi == 0) {
        tpi = 1;
    }
    uint64_t now = *(volatile uint64_t *)HAL_BOARD_MTIME_ADDR;
    mtimecmp_write(hartid, now + tpi);
    if (*(volatile uint64_t *)HAL_BOARD_MTIME_ADDR >= now + tpi) {
        mtimecmp_write(hartid, *(volatile uint64_t *)HAL_BOARD_MTIME_ADDR + tpi);
    }
}

/**
 * @brief 本 hart 定时器硬件初始化
 * @details
 * 1. 忽略 tick_hz（当前实现以 CONFIG_TICK_RATE_HZ 为准）。
 * 2. clint_schedule_next(本 hart) 预约首次 tick。
 * 3. 打开 mie.MTIE 使能本地定时器中断。
 * @param[in] dev     设备描述符；本驱动未使用
 * @param[in] tick_hz 期望频率；板级可忽略
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_timer_init 经 ops->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t clint_hw_init(hal_device_t *dev, uint32_t tick_hz)
{
    (void)dev;
    (void)tick_hz;
    clint_schedule_next(arch_cpu_id());
    set_csr_bits(mie, 0x80);
    return HAL_OK;
}

static const hal_timer_ops_t s_clint_ops = {
    .init       = clint_hw_init,
    .mtime_read = clint_hw_mtime_read,
};

static hal_device_t s_clint_dev = {
    .name      = "clint0",
    .class     = HAL_DEV_TIMER,
    .mmio_base = HAL_BOARD_CLINT_BASE,
    .ops       = &s_clint_ops,
    .priv      = 0,
    .flags     = 0,
};

/**
 * @brief 向 HAL 导出 CLINT/SysTimer 设备描述符
 * @details
 * 1. 返回静态 s_clint_dev 指针。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 勿释放返回值；应用应使用 hal_timer_init / hal_mtime_read
 * @warning 驱动不得自行调用 hal_device_register
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 */
hal_device_t *drv_timer_device(void)
{
    return &s_clint_dev;
}

/**
 * @brief 机器定时器中断 C 入口（底层直调驱动，不经 HAL）
 * @details
 * 1. cgrtos_isr_enter 标记 ISR 上下文。
 * 2. clint_schedule_next 重载本 hart mtimecmp。
 * 3. cgrtos_tick_handler 推进内核 tick。
 * 4. cgrtos_isr_exit 退出 ISR 上下文。
 * @param[in] f 陷阱栈帧指针；本实现未使用
 * @return 无
 * @retval 无
 * @note 由 startup.S / trap 向量直接调用；禁止再绕回 hal_timer_*
 * @warning 须在中断上下文调用；不可从任务直接调用
 * @attention ✅ ISR-safe；✅ 可能引起上下文切换（tick）
 */
void arch_handle_timer(uint64_t *f)
{
    (void)f;
    cgrtos_isr_enter();
    clint_schedule_next(arch_cpu_id());
    cgrtos_tick_handler();
    cgrtos_isr_exit();
}
