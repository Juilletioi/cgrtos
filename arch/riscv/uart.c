/**
 * @file uart.c
 * @brief Nuclei evalsoc UART0 板级驱动（纯硬件层，供 HAL 调用）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * ## 分层位置
 * - 本文件 = Driver：只访问 MMIO，实现 console ops。
 * - 不调用任何 hal_* 用户 API（禁止依赖倒置）。
 * - HAL 通过 drv_uart_device() 取得本设备并注册 / 分发。
 * - 用户请使用 hal_console_* 或兼容层 cgrtos_uart_*（在 hal_compat.c）。
 *
 * ## 硬件要点（SiFive UART）
 * - TXDATA bit31 = TXFULL；RXDATA bit31 = RXEMPTY。
 * - 驱动对 '\\n' 自动补 '\\r'，方便串口终端。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "../../hal/hal_board.h"

#define UART_BASE   HAL_BOARD_UART_BASE
#define UART_TXDATA HAL_BOARD_UART_TXDATA
#define UART_RXDATA HAL_BOARD_UART_RXDATA
#define UART_TXCTRL HAL_BOARD_UART_TXCTRL
#define UART_RXCTRL HAL_BOARD_UART_RXCTRL

/**
 * @brief UART 硬件初始化：使能 TX/RX 通道
 * @details
 * 1. 取 TXCTRL / RXCTRL 寄存器指针。
 * 2. 将 bit0 置 1，分别打开发送与接收。
 * 3. 返回 HAL_OK。
 * @param[in] dev 设备描述符；本驱动未使用 priv
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_console_init 经 ops->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
 */
static hal_status_t uart_hw_init(hal_device_t *dev)
{
    (void)dev;
    volatile uint32_t *txctrl = (volatile uint32_t *)(UART_BASE + UART_TXCTRL);
    volatile uint32_t *rxctrl = (volatile uint32_t *)(UART_BASE + UART_RXCTRL);
    /* 1-2. 使能收发 */
    *txctrl |= 1U;
    *rxctrl |= 1U;
    /* 3. */
    return HAL_OK;
}

/**
 * @brief 阻塞发送一字符到 UART
 * @details
 * 1. 轮询 TXDATA.TXFULL 直至 FIFO 可写。
 * 2. 写入字符低 8 位。
 * 3. 若为 '\\n'，再次等待可写并追加 '\\r'（CRLF）。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] c   待发送字符
 * @return 无
 * @note 由 HAL 在控制台锁内调用；本函数不加锁
 * @warning 轮询等待 TXFULL 清空，可能长时间占用 CPU
 * @attention ✅ ISR；✅ 阻塞（轮询 TX FIFO）
 * @internal
 */
static void uart_hw_putc(hal_device_t *dev, char c)
{
    (void)dev;
    volatile uint32_t *txdata = (volatile uint32_t *)(UART_BASE + UART_TXDATA);
    /* 1. 等待 FIFO 非满 */
    while (*txdata & 0x80000000U) {
    }
    /* 2. 写出 */
    *txdata = (unsigned char)c;
    if (c == '\n') {
        /* 3. CRLF */
        while (*txdata & 0x80000000U) {
        }
        *txdata = (unsigned char)'\r';
    }
}

/**
 * @brief 非阻塞接收一字符
 * @details
 * 1. 读 RXDATA 寄存器。
 * 2. 若 bit31（RXEMPTY）置位，返回 -1。
 * 3. 否则返回低 8 位数据。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return 接收到的字节或 -1
 * @retval 0..255 有效数据
 * @retval -1     RX 空（非 HAL 错误码）
 * @note 由 HAL hal_console_pollc 在锁内调用
 * @warning 返回值 -1 勿与 HAL_ERR_* 混淆
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static int uart_hw_pollc(hal_device_t *dev)
{
    (void)dev;
    volatile uint32_t *rxdata = (volatile uint32_t *)(UART_BASE + UART_RXDATA);
    uint32_t v = *rxdata;
    if (v & 0x80000000U) {
        return -1;
    }
    return (int)(v & 0xFFU);
}

static const hal_console_ops_t s_uart_ops = {
    .init  = uart_hw_init,
    .putc  = uart_hw_putc,
    .pollc = uart_hw_pollc,
};

static hal_device_t s_uart_dev = {
    .name      = "uart0",
    .class     = HAL_DEV_CONSOLE,
    .mmio_base = HAL_BOARD_UART_BASE,
    .ops       = &s_uart_ops,
    .priv      = 0,
    .flags     = 0,
};

/**
 * @brief 向 HAL 导出 UART 控制台设备描述符
 * @details
 * 1. 返回静态 s_uart_dev 指针。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 勿释放返回值；应用应使用 hal_console_*
 * @warning 驱动不得自行调用 hal_device_register
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *drv_uart_device(void)
{
    return &s_uart_dev;
}

/**
 * @brief 极早期 / trap 诊断用单字符输出（直写 MMIO，不经 HAL）
 * @details
 * 1. 委托 uart_hw_putc 直写 UART MMIO。
 * 2. '\\n' 自动补 '\\r'。
 * @param[in] c 待输出字符
 * @return 无
 * @note 仅供异常诊断等底层路径；应用请用 hal_console_*
 * @warning 无锁、轮询阻塞；多核并发输出可能字节交错
 * @attention ✅ ISR；✅ 阻塞（轮询 TX FIFO）
 */
void drv_uart_early_putc(char c)
{
    uart_hw_putc(&s_uart_dev, c);
}

/**
 * @brief 极早期 / trap 诊断用字符串输出（直写 MMIO，不经 HAL）
 * @details
 * 1. s 为 NULL 时直接返回。
 * 2. 逐字符调用 drv_uart_early_putc 输出。
 * @param[in] s NUL 结尾字符串；NULL 忽略
 * @return 无
 * @note 仅供异常诊断等底层路径；应用请用 hal_console_puts
 * @warning 无锁、轮询阻塞；多核并发输出可能字节交错
 * @attention ✅ ISR；✅ 阻塞（轮询 TX FIFO）
 */
void drv_uart_early_puts(const char *s)
{
    if (!s) {
        return;
    }
    while (*s) {
        drv_uart_early_putc(*s++);
    }
}
