/**
 * @file hal_drv.h
 * @brief HAL ↔ 板级驱动绑定接口（仅供 HAL 与 arch 驱动使用，应用勿包含）
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
 *   板级 Driver（arch/riscv 下各 .c：只碰 MMIO）
 * @endcode
 *
 * - Driver **不得** 调用任何 `hal_*` 用户 API。
 * - Driver 只实现 ops，并通过 `drv_*_device()` 把静态 `hal_device_t` 交给 HAL。
 * - HAL 的 `hal_board_init()` 负责 `hal_device_register()` 与按序 init。
 * - Trap / 极底层 ISR 入口应直接调用本文件对应驱动内部例程（或同编译单元 static），
 *   **禁止** 为图省事再绕回 `hal_irqc_claim()` 等用户 API。
 */
#ifndef CGRTOS_HAL_DRV_H
#define CGRTOS_HAL_DRV_H

#include "hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 取得 UART 控制台设备描述符（静态寿命）
 * @return 非 NULL；供 HAL 注册，勿释放
 *
 * @details
 * 1. 驱动在 .data 中放置唯一 `hal_device_t` + `hal_console_ops_t`。
 * 2. 本函数仅返回指针，**不**调用 `hal_device_register`。
 * 3. 由 `hal_board_init` 统一注册。
 */
hal_device_t *drv_uart_device(void);

/**
 * @brief 取得 CLINT/SysTimer 设备描述符
 * @return 非 NULL
 */
hal_device_t *drv_clint_device(void);

/**
 * @brief 取得 PLIC 设备描述符
 * @return 非 NULL
 */
hal_device_t *drv_plic_device(void);

/**
 * @brief 取得 IPI（MSIP）设备描述符
 * @return 非 NULL
 */
hal_device_t *drv_ipi_device(void);

/**
 * @brief 取得 CPU 早期初始化设备描述符
 * @return 非 NULL
 */
hal_device_t *drv_cpu_device(void);

/**
 * @brief 极早期 / trap 诊断 putc（直写 UART MMIO，禁止经 HAL）
 */
void drv_uart_early_putc(char c);

/** @brief 极早期 puts */
void drv_uart_early_puts(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* CGRTOS_HAL_DRV_H */
