/**
 * @file clint.c
 * @brief Nuclei SysTimer / CLINT 定时器驱动（纯硬件层）
 *
 * @details
 * - 提供 timer ops；trap 入口 riscv_handle_timer 直接调本文件静态例程。
 * - 禁止调用 hal_*；用户读 mtime 请用 hal_mtime_read()。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "../../hal/hal_board.h"

#ifndef CONFIG_TIMER_CLOCK_HZ
#define CONFIG_TIMER_CLOCK_HZ  1000000ULL
#endif

extern void cgrtos_isr_enter(void);
extern void cgrtos_isr_exit(void);

/**
 * @brief 读 mtime 自由运行计数器
 * @details 步骤：1. volatile 读 HAL_BOARD_MTIME_ADDR；2. 返回 64 位值。
 */
static uint64_t clint_hw_mtime_read(hal_device_t *dev)
{
    (void)dev;
    return *(volatile uint64_t *)HAL_BOARD_MTIME_ADDR;
}

/**
 * @brief 写本 hart mtimecmp 并 fence
 *
 * @details 步骤：
 * 1. 按 hartid 计算 MTIMECMP 地址。
 * 2. 写入比较值。
 * 3. fence iorw 保证对后续 mtime 可见。
 */
static inline void mtimecmp_write(uint64_t hartid, uint64_t val)
{
    *(volatile uint64_t *)HAL_BOARD_MTIMECMP_ADDR(hartid) = val;
    asm volatile("fence iorw, iorw" ::: "memory");
}

/**
 * @brief 按 CONFIG 频率预约下一次 MTIP
 *
 * @details 步骤：
 * 1. tpi = TIMER_CLOCK_HZ / TICK_RATE_HZ（至少 1）。
 * 2. now = mtime；写入 mtimecmp = now + tpi。
 * 3. 若写后 mtime 已越过比较值，基于最新 mtime 重写，避免丢中断。
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
 * @brief 本 hart 定时器 init
 *
 * @param tick_hz 期望频率（板级可忽略，用 CONFIG_TICK_RATE_HZ）
 * @return HAL_OK
 *
 * @details 步骤：
 * 1. 忽略或记录 tick_hz（当前实现以 CONFIG 为准）。
 * 2. clint_schedule_next(本 hart)。
 * 3. 打开 mie.MTIE。
 */
static hal_status_t clint_hw_init(hal_device_t *dev, uint32_t tick_hz)
{
    (void)dev;
    (void)tick_hz;
    clint_schedule_next(read_csr(mhartid));
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

hal_device_t *drv_clint_device(void)
{
    return &s_clint_dev;
}

/**
 * @brief 机器定时器中断 C 入口（底层：直调驱动，不经 HAL）
 *
 * @details 步骤：
 * 1. cgrtos_isr_enter。
 * 2. 重载本 hart mtimecmp。
 * 3. cgrtos_tick_handler 推进内核 tick。
 * 4. cgrtos_isr_exit。
 */
void riscv_handle_timer(uint64_t *f)
{
    (void)f;
    cgrtos_isr_enter();
    clint_schedule_next(read_csr(mhartid));
    cgrtos_tick_handler();
    cgrtos_isr_exit();
}
