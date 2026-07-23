/**
 * @file arch.c
 * @brief AArch64 CPU 早期初始化（打开 IRQ 屏蔽以外的使能由 GIC/timer 完成）
 */
#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

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

hal_device_t *drv_cpu_device(void)
{
    return &s_cpu_dev;
}
