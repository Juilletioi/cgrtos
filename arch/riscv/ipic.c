/**
 * @file ipic.c
 * @brief CLINT MSIP 核间中断驱动（纯硬件层）+ IPI trap 入口
 *
 * @details
 * - ops 供 HAL 的 hal_ipi_send / clear。
 * - riscv_handle_ipi 直接 ipi_hw_clear，不经 HAL。
 * - cgrtos_smp_send_ipi 兼容封装在 hal_compat.c。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "../../hal/hal_board.h"

extern void cgrtos_isr_enter(void);
extern void cgrtos_isr_exit(void);

/** @brief IPI 设备 init（MSIP 无需额外配置） */
static hal_status_t ipi_hw_init(hal_device_t *dev)
{
    (void)dev;
    return HAL_OK;
}

/**
 * @brief 向目标 hart 写 MSIP=1
 *
 * @details 步骤：
 * 1. 校验 hart < CONFIG_NUM_CORES。
 * 2. 写 MSIP(hart)=1。
 * 3. 内存屏障。
 * 4. 返回 HAL_OK / HAL_ERR_PARAM。
 */
static hal_status_t ipi_hw_send(hal_device_t *dev, uint8_t hart)
{
    (void)dev;
    if (hart >= CONFIG_NUM_CORES) {
        return HAL_ERR_PARAM;
    }
    volatile uint32_t *msip =
        (volatile uint32_t *)(unsigned long)HAL_BOARD_CLINT_MSIP(hart);
    *msip = 1;
    __sync_synchronize();
    return HAL_OK;
}

/**
 * @brief 清目标 hart MSIP
 * @details 步骤：1. 计算 MSIP 地址；2. 写 0。
 */
static void ipi_hw_clear(hal_device_t *dev, uint8_t hart)
{
    (void)dev;
    volatile uint32_t *msip =
        (volatile uint32_t *)(unsigned long)HAL_BOARD_CLINT_MSIP(hart);
    *msip = 0;
}

static const hal_ipi_ops_t s_ipi_ops = {
    .init  = ipi_hw_init,
    .send  = ipi_hw_send,
    .clear = ipi_hw_clear,
};

static hal_device_t s_ipi_dev = {
    .name      = "ipi0",
    .class     = HAL_DEV_IPI,
    .mmio_base = HAL_BOARD_CLINT_BASE,
    .ops       = &s_ipi_ops,
    .priv      = 0,
    .flags     = 0,
};

hal_device_t *drv_ipi_device(void)
{
    return &s_ipi_dev;
}

/**
 * @brief 机器软件中断 C 入口（底层直调驱动）
 *
 * @details 步骤：
 * 1. cgrtos_isr_enter。
 * 2. 读 mhartid，ipi_hw_clear 本核 MSIP。
 * 3. 若 g_remote_tick[h]：清标志并 cgrtos_tick_local。
 * 4. 置 g_yield_pending[h]=1。
 * 5. cgrtos_isr_exit。
 */
void riscv_handle_ipi(uint64_t *f)
{
    (void)f;
    cgrtos_isr_enter();

    uint64_t h = read_csr(mhartid);
    ipi_hw_clear(&s_ipi_dev, (uint8_t)h);

    if (h < CONFIG_NUM_CORES && g_remote_tick[h]) {
        g_remote_tick[h] = 0;
        cgrtos_tick_local();
    }

    g_yield_pending[h] = 1;
    cgrtos_isr_exit();
}
