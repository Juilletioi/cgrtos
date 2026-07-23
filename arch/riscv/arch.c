/**
 * @file arch.c
 * @brief RISC-V CPU 早期初始化驱动（纯硬件层）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * - 只置 mie 中 MSIE|MTIE|MEIE；不调用任何 hal_* API。
 * - HAL 经 drv_cpu_device() 取得设备后执行 ops->init。
 * - 栈 / mtvec / I-cache 已在 startup.S 完成。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

/**
 * @brief 打开三类 M 模式中断使能位
 * @details
 * 1. set_csr_bits(mie, 0x888)：
 *    - bit3  MSIE 软件中断（IPI）
 *    - bit7  MTIE 定时器中断
 *    - bit11 MEIE 外部中断（PLIC）
 * 2. 返回 HAL_OK（置位幂等，可重复调用）。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_cpu_init 经 ops->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
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
 * @brief 向 HAL 导出 CPU 设备描述符
 * @details
 * 1. 返回静态 s_cpu_dev 指针。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 勿释放返回值；应用应使用 hal_cpu_init
 * @warning 驱动不得自行调用 hal_device_register
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *drv_cpu_device(void)
{
    return &s_cpu_dev;
}
