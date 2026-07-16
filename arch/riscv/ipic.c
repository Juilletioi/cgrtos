/**
 * @file ipic.c
 * @brief SMP 核间软件中断（IPI）驱动。
 *
 * 通过 CLINT MSIP 寄存器向目标 hart 发送调度唤醒 IPI。
 * 次核还可借 IPI 执行 hart0 发起的远程 tick（时间片记账）。
 */

#include "../../kernel/cgrtos.h"

/** @brief 指定 hart 的 CLINT MSIP 寄存器地址。 */
#define CLINT_MSIP(h) (0x18031000 + (h) * 4)

extern void cgrtos_isr_enter(void);
extern void cgrtos_isr_exit(void);

/**
 * @brief 向目标核心发送软件 IPI。
 *
 * @param core 目标 hart 编号（须 < CONFIG_NUM_CORES）。
 *
 * @details
 * 1. 校验 core 是否在有效核数范围内，越界则直接返回。
 * 2. 计算目标 hart 的 CLINT MSIP 寄存器地址。
 * 3. 写入 MSIP=1，触发目标 hart 的机器软件中断（MSI）。
 * 4. 执行 __sync_synchronize 内存屏障，确保写入对目标核可见。
 *
 * @note 目标 hart 在 riscv_handle_ipi 中清 MSIP 并置 yield_pending。
 */
void cgrtos_smp_send_ipi(uint8_t core)
{
    /* 1. 校验目标核编号 */
    if (core >= CONFIG_NUM_CORES) {
        return;
    }
    /* 2. 计算 MSIP 寄存器地址 */
    volatile uint32_t *msip = (volatile uint32_t *)(unsigned long)CLINT_MSIP(core);
    /* 3. 写入 MSIP=1 触发软件中断 */
    *msip = 1;
    /* 4. 内存屏障确保目标核可见 */
    __sync_synchronize();
}

/**
 * @brief 机器软件中断（IPI）C 层入口。
 *
 * @param f 陷阱栈帧指针（未使用）。
 *
 * @details
 * 1. 调用 cgrtos_isr_enter 进入 ISR 上下文。
 * 2. 读取本 hart 编号 h，清除本核 MSIP（写 0），避免重复触发。
 * 3. 若 g_remote_tick[h] 置位，说明 hart0 请求本核执行本地 tick：
 *    a. 清除 g_remote_tick[h]；
 *    b. 调用 cgrtos_tick_local 做时间片记账与抢占评估。
 * 4. 置 g_yield_pending[h]=1，在 trap 返回路径触发调度切换。
 * 5. 调用 cgrtos_isr_exit 退出 ISR 上下文。
 */
void riscv_handle_ipi(uint64_t *f)
{
    (void)f;
    /* 1. 进入 ISR 上下文 */
    cgrtos_isr_enter();

    /* 2. 读取 hart 编号并清除 MSIP */
    uint64_t h = read_csr(mhartid);
    volatile uint32_t *msip = (volatile uint32_t *)CLINT_MSIP(h);
    *msip = 0;

    /* 3. hart0 发起的远程 tick 处理 */
    if (h < CONFIG_NUM_CORES && g_remote_tick[h]) {
        g_remote_tick[h] = 0;
        cgrtos_tick_local();
    }

    /* 4. 请求调度器在 trap 返回时切换 */
    g_yield_pending[h] = 1;

    /* 5. 退出 ISR 上下文 */
    cgrtos_isr_exit();
}
