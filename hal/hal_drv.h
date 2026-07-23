/**
 * @file hal_drv.h
 * @brief HAL ↔ 板级驱动绑定接口（仅供 HAL 与 arch 驱动使用，应用勿包含）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * ## 正确依赖方向（禁止倒置）
 * @code
 *   应用 / 内核业务
 *         |
 *         v
 *      HAL API（hal.h：hal_console_* /hal_irqc_* …）
 *         |
 *         |  调用 ops / 注册 device
 *         v
 *   板级 Driver（arch/<ARCH> 下各 .c：只碰 MMIO）
 * @endcode
 *
 * - Driver **不得** 调用任何 `hal_*` 用户 API。
 * - Driver 只实现 ops，并通过 `drv_*_device()` 把静态 `hal_device_t` 交给 HAL。
 * - HAL 的 `hal_board_init()` 负责 `hal_device_register()` 与按序 init。
 * - Trap / 极底层 ISR 入口应直接调用本文件对应驱动内部例程（或同编译单元 static），
 *   **禁止** 为图省事再绕回 `hal_irqc_claim()` 等用户 API。
 *
 * 中性设备名：`drv_timer_device` / `drv_irqc_device`；
 * 兼容旧名：`drv_clint_device` / `drv_plic_device`（宏别名）。
 */
#ifndef CGRTOS_HAL_DRV_H
#define CGRTOS_HAL_DRV_H

#include "hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 取得 UART 控制台设备描述符（静态寿命）
 * @details
 * 1. 驱动在 .data 中放置唯一 hal_device_t + hal_console_ops_t。
 * 2. 本函数仅返回指针，**不**调用 hal_device_register。
 * 3. 由 hal_board_init 统一注册。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 应用应使用 hal_console_*，勿直接包含本头
 * @warning 驱动不得自行注册或释放
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *drv_uart_device(void);

/**
 * @brief 取得系统定时器设备描述符（CLINT/SysTimer 或 CNTV 等）
 * @details 注册由 hal_board_init 完成；应用用 hal_timer_init / hal_mtime_read。
 * @return 非 NULL 静态设备指针
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *drv_timer_device(void);

/** @brief 兼容旧名（RISC-V CLINT 时代） */
#define drv_clint_device drv_timer_device

/**
 * @brief 取得外部中断控制器设备描述符（PLIC 或 GIC 等）
 * @details 应用应使用 hal_irqc_*；trap 路径直调驱动 static 例程。
 * @return 非 NULL 静态设备指针
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *drv_irqc_device(void);

/** @brief 兼容旧名（RISC-V PLIC 时代） */
#define drv_plic_device drv_irqc_device

/**
 * @brief 取得 IPI 设备描述符（MSIP / SGI 等）
 * @details
 * 1. 返回静态 IPI 设备指针。
 * 2. 注册由 hal_board_init 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 应用应使用 hal_ipi_send / hal_ipi_clear
 * @warning 驱动不得自行注册或释放
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *drv_ipi_device(void);

/**
 * @brief 取得 CPU 早期初始化设备描述符
 * @details
 * 1. 返回静态 CPU 设备指针。
 * 2. 注册由 hal_board_init 完成；ops->init 打开架构相关中断使能。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 应用应使用 hal_cpu_init
 * @warning 驱动不得自行注册或释放
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *drv_cpu_device(void);

/**
 * @brief 极早期 / trap 诊断用单字符输出（直写 UART MMIO，禁止经 HAL）
 * @details
 * 1. 直写 UART TXDATA MMIO，轮询 TXFULL 直至可写。
 * 2. '\\n' 自动补 '\\r'。
 * @param[in] c 待输出字符
 * @return 无
 * @note 仅供异常诊断等底层路径；应用请用 hal_console_putc
 * @warning 无锁、轮询阻塞；多核并发输出可能字节交错
 * @attention ✅ ISR；✅ 阻塞（轮询 TX FIFO）
 */
void drv_uart_early_putc(char c);

/**
 * @brief 极早期 / trap 诊断用字符串输出（直写 UART MMIO，禁止经 HAL）
 * @details
 * 1. s 为 NULL 时忽略。
 * 2. 逐字符调用 drv_uart_early_putc 输出。
 * @param[in] s NUL 结尾字符串；NULL 忽略
 * @return 无
 * @note 仅供异常诊断等底层路径；应用请用 hal_console_puts
 * @warning 无锁、轮询阻塞；多核并发输出可能字节交错
 * @attention ✅ ISR；✅ 阻塞（轮询 TX FIFO）
 */
void drv_uart_early_puts(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* CGRTOS_HAL_DRV_H */
