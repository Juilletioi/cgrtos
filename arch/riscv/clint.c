/**
 * @file clint.c
 * @brief Nuclei evalsoc SysTimer 与 CLINT 定时器驱动。
 *
 * 提供 mtime/mtimecmp 读写及机器定时器中断处理。
 * 基址：mtime @ 0x18030000，mtimecmp/MSIP @ 0x18031000。
 * timebase-frequency = 1 MHz。
 */

#include "../../kernel/cgrtos.h"

/** @brief SysTimer 寄存器基址。 */
#define SYSTIMER_BASE     0x18030000UL
/** @brief CLINT 寄存器基址（mtimecmp、MSIP）。 */
#define CLINT_BASE        0x18031000UL
/** @brief mtime 只读计数器地址。 */
#define MTIME_ADDR        (SYSTIMER_BASE + 0x0UL)
/** @brief 指定 hart 的 mtimecmp 比较寄存器地址。 */
#define MTIMECMP_ADDR(h)  (CLINT_BASE + 0x4000UL + (unsigned long)(h) * 8UL)

#ifndef CONFIG_TIMER_CLOCK_HZ
/** @brief 定时器时钟频率（Hz），默认 1 MHz。 */
#define CONFIG_TIMER_CLOCK_HZ  1000000ULL
#endif

extern void cgrtos_isr_enter(void);
extern void cgrtos_isr_exit(void);

/**
 * @brief 读取 mtime 自由运行计数器。
 *
 * @return 当前 mtime 值（64 位无符号）。
 *
 * @details
 * 1. 通过 volatile 指针直接访问 SYSTIMER 基址处的 mtime 寄存器。
 * 2. 返回 64 位计数值，供 tick 调度与 mtimecmp 比较使用。
 */
uint64_t cgrtos_mtime_read(void)
{
    /* 1. volatile 读取 mtime 寄存器 */
    /* 2. 返回 64 位计数值 */
    return *(volatile uint64_t *)MTIME_ADDR;
}

/**
 * @brief 写入指定 hart 的 mtimecmp 并执行内存屏障。
 *
 * @param hartid 目标 hart 编号。
 * @param val    下一次比较匹配值（mtime 达到此值时触发 MTI）。
 *
 * @details
 * 1. 根据 hartid 计算该核 mtimecmp 寄存器 MMIO 地址。
 * 2. 写入比较值 val。
 * 3. 执行 fence iorw, iorw 确保写入对后续 mtime 读取可见。
 */
static inline void mtimecmp_write(uint64_t hartid, uint64_t val)
{
    /* 1. 写入 hart 对应的 mtimecmp */
    *(volatile uint64_t *)MTIMECMP_ADDR(hartid) = val;
    /* 3. 内存屏障确保写入可见 */
    asm volatile("fence iorw, iorw" ::: "memory");
}

/**
 * @brief 根据 tick 频率调度下一次定时器中断。
 *
 * @param hartid 目标 hart 编号。
 *
 * @details
 * 1. 计算每 tick 对应的 mtime 增量 tpi = TIMER_CLOCK_HZ / TICK_RATE_HZ，最小为 1。
 * 2. 读取当前 mtime 作为基准 now。
 * 3. 将 mtimecmp 设为 now + tpi，预约下一次中断。
 * 4. 若写入后 mtime 已越过比较值（竞态），则基于最新 mtime 重新设置，避免丢失中断。
 */
static void clint_schedule_next(uint64_t hartid)
{
    /* 1. 计算每 tick 的 mtime 增量 */
    uint64_t tpi = CONFIG_TIMER_CLOCK_HZ / CONFIG_TICK_RATE_HZ;
    if (tpi == 0) {
        tpi = 1;
    }

    /* 2-3. 读取当前时间并写入 mtimecmp */
    uint64_t now = cgrtos_mtime_read();
    mtimecmp_write(hartid, now + tpi);

    /* 4. 竞态修复——mtime 已越过比较值则重设 */
    if (cgrtos_mtime_read() >= now + tpi) {
        mtimecmp_write(hartid, cgrtos_mtime_read() + tpi);
    }
}

/**
 * @brief 初始化 CLINT 定时器并使能 MTIE。
 *
 * @param rate 期望 tick 频率（当前未使用，由 CONFIG_TICK_RATE_HZ 决定）。
 *
 * @details
 * 1. 忽略 rate 参数，实际周期由 CONFIG_TICK_RATE_HZ 与 CONFIG_TIMER_CLOCK_HZ 决定。
 * 2. 为本 hart 调用 clint_schedule_next 设置首次 mtimecmp。
 * 3. 通过 set_csr_bits 打开 mie 的 MTIE 位（bit 7），允许机器定时器中断。
 */
void cgrtos_clint_init(tick_t rate)
{
    /* 1. rate 未使用，周期由 CONFIG 决定 */
    (void)rate;
    /* 2. 设置本 hart 首次 mtimecmp */
    clint_schedule_next(read_csr(mhartid));
    /* 3. 打开 MTIE 允许定时器中断 */
    set_csr_bits(mie, 0x80); /* MTIE */
}

/**
 * @brief 机器定时器中断 C 层入口。
 *
 * @param f 陷阱栈帧指针（未使用，由汇编陷阱向量传入）。
 *
 * @details
 * 1. 调用 cgrtos_isr_enter 递增 ISR 嵌套计数。
 * 2. 重载本 hart 的 mtimecmp，预约下一次 tick 中断。
 * 3. 调用 cgrtos_tick_handler 推进内核全局/本地 tick 逻辑。
 * 4. 调用 cgrtos_isr_exit 递减 ISR 嵌套计数。
 */
void riscv_handle_timer(uint64_t *f)
{
    (void)f;
    /* 1. 进入 ISR 上下文 */
    cgrtos_isr_enter();
    /* 2. 重载 mtimecmp 预约下次中断 */
    clint_schedule_next(read_csr(mhartid));
    /* 3. 推进内核 tick 逻辑 */
    cgrtos_tick_handler();
    /* 4. 退出 ISR 上下文 */
    cgrtos_isr_exit();
}
