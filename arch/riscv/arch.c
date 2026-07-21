/**
 * @file arch.c
 * @brief RISC-V CPU 早期初始化驱动（纯硬件层）
 *
 * @details
 * - 只置 mie 中 MSIE|MTIE|MEIE；不调用任何 hal_* API。
 * - HAL 经 drv_cpu_device() 取得设备后执行 ops->init。
 * - 栈 / mtvec / I-cache 已在 startup.S 完成。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "../../hal/hal_board.h"

/**
 * @brief 打开三类 M 模式中断使能
 *
 * @param dev 未使用
 * @return HAL_OK
 *
 * @details 步骤：
 * 1. set_csr_bits(mie, 0x888)：
 *    - bit3  MSIE 软件中断（IPI）
 *    - bit7  MTIE 定时器中断
 *    - bit11 MEIE 外部中断（PLIC）
 * 2. 返回 HAL_OK（置位幂等，可重复调用）。
 */
static hal_status_t cpu_hw_init(hal_device_t *dev)
{
    (void)dev;
    set_csr_bits(mie, 0x888);
    return HAL_OK;
}

static const hal_cpu_ops_t s_cpu_ops = {
    .init = cpu_hw_init,
};

static hal_device_t s_cpu_dev = {
    .name      = "cpu0",
    .class     = HAL_DEV_CPU,
    .mmio_base = 0,
    .ops       = &s_cpu_ops,
    .priv      = 0,
    .flags     = 0,
};

/**
 * @brief 向 HAL 导出 CPU 设备
 * @return &s_cpu_dev
 */
hal_device_t *drv_cpu_device(void)
{
    return &s_cpu_dev;
}
