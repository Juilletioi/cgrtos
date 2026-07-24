/**
 * @file ipic.c
 * @brief AArch64 IPI 占位驱动（单核 virt 首版；后续可接 GIC SGI）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * - ops 供 HAL 的 hal_ipi_send / clear。
 * - 当前 send/clear 为空操作（TODO：SMP 时写 ICC_SGI1R_EL1）。
 * - 不调用任何 hal_* 用户 API。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

/**
 * @brief IPI 设备硬件占位初始化
 * @details
 * 1. 忽略 dev。
 * 2. 直接返回 HAL_OK（无 MMIO 需配置）。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_board_init 经 ops-->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t ipi_hw_init(hal_device_t *dev)
{
    (void)dev;
    return HAL_OK;
}

/**
 * @brief 向目标 CPU 发送 IPI（占位：校验后成功返回）
 * @details
 * 1. 校验 hart 小于 CONFIG_NUM_CORES。
 * 2. TODO：SMP 启用后写 ICC_SGI1R_EL1。
 * 3. 当前直接返回 HAL_OK。
 * @param[in] dev  设备描述符；本驱动未使用
 * @param[in] hart 目标逻辑 CPU 编号
 * @return HAL_OK 或 HAL_ERR_PARAM
 * @retval HAL_OK        成功（占位）
 * @retval HAL_ERR_PARAM hart 越界
 * @note 由 HAL hal_ipi_send 经 ops-->send 调用
 * @warning 单核首版不会真正触发远端中断
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t ipi_hw_send(hal_device_t *dev, uint8_t hart)
{
    (void)dev;
    if (hart >= CONFIG_NUM_CORES) {
        return HAL_ERR_PARAM;
    }
    /* TODO: ICC_SGI1R_EL1 when SMP enabled */
    return HAL_OK;
}

/**
 * @brief 清目标 CPU 的 IPI 挂起（占位空操作）
 * @details
 * 1. 忽略 dev / hart。
 * 2. 无硬件状态可清，直接返回。
 * @param[in] dev  设备描述符；本驱动未使用
 * @param[in] hart 目标逻辑 CPU 编号；本占位未使用
 * @return 无
 * @retval 无
 * @note 供 HAL ops-->clear；SGI 通常由硬件自动完成
 * @warning 无
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static void ipi_hw_clear(hal_device_t *dev, uint8_t hart)
{
    (void)dev;
    (void)hart;
}

static const hal_ipi_ops_t s_ipi_ops = {
    .init = ipi_hw_init,
    .send = ipi_hw_send,
    .clear = ipi_hw_clear,
};

static hal_device_t s_ipi_dev = {
    .name = "ipi0",
    .class = HAL_DEV_IPI,
    .mmio_base = 0,
    .ops = &s_ipi_ops,
    .priv = 0,
    .flags = 0,
};

/**
 * @brief 向 HAL 导出 IPI 设备描述符
 * @details
 * 1. 返回静态 s_ipi_dev 指针。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 勿释放返回值；应用应使用 hal_ipi_send / hal_ipi_clear
 * @warning 驱动不得自行调用 hal_device_register
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 */
hal_device_t *drv_ipi_device(void)
{
    return &s_ipi_dev;
}
