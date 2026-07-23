/**
 * @file ipic.c
 * @brief CLINT MSIP 核间中断驱动（纯硬件层）+ IPI trap 入口
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * - ops 供 HAL 的 hal_ipi_send / clear。
 * - riscv_handle_ipi 直接 ipi_hw_clear，不经 HAL。
 * - cgrtos_smp_send_ipi 兼容封装在 hal_compat.c。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

extern void cgrtos_isr_enter(void);
extern void cgrtos_isr_exit(void);
void board_ipi_clear(uint8_t hart);

/**
 * @brief IPI（MSIP）设备硬件初始化
 * @details
 * 1. MSIP 无需额外硬件配置。
 * 2. 直接返回 HAL_OK。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_board_init 经 ops->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
 */
static hal_status_t ipi_hw_init(hal_device_t *dev)
{
    (void)dev;
    return HAL_OK;
}

/**
 * @brief 向目标 hart 写 MSIP=1 触发软件中断
 * @details
 * 1. 校验 hart < CONFIG_NUM_CORES。
 * 2. 写 HAL_BOARD_CLINT_MSIP(hart) = 1。
 * 3. __sync_synchronize 内存屏障。
 * 4. 返回 HAL_OK 或 HAL_ERR_PARAM。
 * @param[in] dev  设备描述符；本驱动未使用
 * @param[in] hart 目标 hart 编号
 * @return HAL_OK 或 HAL_ERR_PARAM
 * @retval HAL_OK        成功
 * @retval HAL_ERR_PARAM hart 越界
 * @note 由 HAL hal_ipi_send 经 ops->send 调用
 * @warning 目标 hart 须已上线且 MSIE 已使能
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
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
 * @brief 清目标 hart 的 MSIP 挂起位
 * @details
 * 1. 计算 HAL_BOARD_CLINT_MSIP(hart) 地址。
 * 2. 写 0 清除软件中断挂起。
 * @param[in] dev  设备描述符；本驱动未使用
 * @param[in] hart 目标 hart 编号
 * @return 无
 * @note 通常在本核 IPI ISR 中清本核 MSIP
 * @warning hart 须合法；越界写可能导致未定义行为
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static void ipi_hw_clear(hal_device_t *dev, uint8_t hart)
{
    (void)dev;
    board_ipi_clear(hart);
}

/**
 * @brief 清指定 hart 的 MSIP（板级钩子，供 trap 向量调用）
 * @details 写 HAL_BOARD_CLINT_MSIP(hart)=0，避免 startup.S 硬编码 CLINT 基址。
 * @param[in] hart 逻辑 hart 编号
 * @return 无
 * @retval 无
 * @note 由 trap handle_ipi 在 riscv_handle_ipi 前调用
 * @warning hart 越界则忽略
 * @attention ✅ ISR；❌ 不阻塞
 */
void board_ipi_clear(uint8_t hart)
{
    if (hart >= CONFIG_NUM_CORES) {
        return;
    }
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

/**
 * @brief 向 HAL 导出 IPI（MSIP）设备描述符
 * @details
 * 1. 返回静态 s_ipi_dev 指针。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 勿释放返回值；应用应使用 hal_ipi_send / hal_ipi_clear
 * @warning 驱动不得自行调用 hal_device_register
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *drv_ipi_device(void)
{
    return &s_ipi_dev;
}

/**
 * @brief 机器软件中断 C 入口（底层直调驱动，不经 HAL）
 * @details
 * 1. cgrtos_isr_enter 标记 ISR 上下文。
 * 2. 读 mhartid，ipi_hw_clear 清本核 MSIP。
 * 3. 若 g_remote_tick[h] 置位：清标志并 cgrtos_tick_local。
 * 4. 置 g_yield_pending[h]=1 请求调度。
 * 5. cgrtos_isr_exit 退出 ISR 上下文。
 * @param[in] f 陷阱栈帧指针；本实现未使用
 * @return 无
 * @note 由 startup.S / trap 向量直接调用；禁止再绕回 hal_ipi_*
 * @warning 须在中断上下文调用；不可从任务直接调用
 * @attention ✅ ISR；❌ 不阻塞
 */
void arch_handle_ipi(uint64_t *f)
{
    (void)f;
    cgrtos_isr_enter();

    uint64_t h = arch_cpu_id();
    ipi_hw_clear(&s_ipi_dev, (uint8_t)h);

    if (h < CONFIG_NUM_CORES && g_remote_tick[h]) {
        g_remote_tick[h] = 0;
        cgrtos_tick_local();
    }

    g_yield_pending[h] = 1;
    cgrtos_isr_exit();
}
