/**
 * @file arch.c
 * @brief AArch64 CPU 早期初始化驱动（纯硬件层）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * - IRQ/FIQ 仍由 DAIF 控制；向量表已在 startup.S 设置。
 * - 打开 IRQ 屏蔽以外的使能由 GIC / timer 完成。
 * - 不调用任何 hal_* 用户 API；HAL 经 drv_cpu_device() 取得设备后执行 ops-->init。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

/**
 * @brief AArch64 CPU 硬件占位初始化
 * @details
 * 1. 忽略 dev（无 MMIO 配置）。
 * 2. 直接返回 HAL_OK（向量 / 栈已在 startup.S 完成）。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_cpu_init 经 ops-->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t cpu_hw_init(hal_device_t *dev)
{
    (void)dev;
    /* IRQ/FIQ 仍由 DAIF 控制；向量已在 startup.S 设置 */
    return HAL_OK;
}

static const hal_cpu_ops_t s_cpu_ops = {
    .init = cpu_hw_init,
};

static hal_device_t s_cpu_dev = {
    .name = "cpu0",
    .class = HAL_DEV_CPU,
    .mmio_base = 0,
    .ops = &s_cpu_ops,
    .priv = 0,
    .flags = 0,
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
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 */
hal_device_t *drv_cpu_device(void)
{
    return &s_cpu_dev;
}
