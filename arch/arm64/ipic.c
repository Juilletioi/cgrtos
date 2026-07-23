/**
 * @file ipic.c
 * @brief AArch64 IPI 占位（单核 virt 首版；后续可接 GIC SGI）
 */
#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

static hal_status_t ipi_hw_init(hal_device_t *dev)
{
    (void)dev;
    return HAL_OK;
}

static hal_status_t ipi_hw_send(hal_device_t *dev, uint8_t hart)
{
    (void)dev;
    if (hart >= CONFIG_NUM_CORES) {
        return HAL_ERR_PARAM;
    }
    /* TODO: ICC_SGI1R_EL1 when SMP enabled */
    return HAL_OK;
}

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

hal_device_t *drv_ipi_device(void)
{
    return &s_ipi_dev;
}
