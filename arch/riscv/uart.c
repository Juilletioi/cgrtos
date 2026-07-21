/**
 * @file uart.c
 * @brief Nuclei evalsoc UART0 板级驱动（纯硬件层，供 HAL 调用）
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
 * @brief 硬件初始化：使能 TX/RX 通道
 *
 * @param dev 设备描述符（本驱动未使用 priv）
 * @return HAL_OK
 *
 * @details 步骤：
 * 1. 取 TXCTRL / RXCTRL 寄存器指针。
 * 2. 将 bit0 置 1，分别打开发送与接收。
 * 3. 返回 HAL_OK。
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
 * @brief 阻塞发送一字符
 *
 * @param dev 设备
 * @param c  待发送字符
 *
 * @details 步骤：
 * 1. 轮询 TXDATA.TXFULL 直至可写。
 * 2. 写入字符低 8 位。
 * 3. 若为 '\\n'，再次等待可写并追加 '\\r'（CRLF）。
 *
 * @note 由 HAL 在控制台锁内调用；本函数不加锁。
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
 *
 * @return 0..255 数据；-1 表示 RX 空（非 HAL 错误码）
 *
 * @details 步骤：
 * 1. 读 RXDATA。
 * 2. 若 bit31（RXEMPTY）置位，返回 -1。
 * 3. 否则返回低 8 位。
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
 * @brief 向 HAL 导出 UART 设备（不注册）
 *
 * @return &s_uart_dev
 *
 * @details 步骤：
 * 1. 返回静态设备地址。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 */
hal_device_t *drv_uart_device(void)
{
    return &s_uart_dev;
}

/**
 * @brief 极早期 / trap 诊断用 putc（直写 MMIO，不经 HAL）
 *
 * @param c 字符；'\n' 自动补 '\r'
 *
 * @details 步骤：
 * 1. 轮询 TXFULL 直至可写。
 * 2. 写字符。
 * 3. 若为换行则再写 '\r'。
 *
 * @note 仅供异常诊断等底层路径；应用请用 hal_console_*。
 */
void drv_uart_early_putc(char c)
{
    uart_hw_putc(&s_uart_dev, c);
}

/**
 * @brief 极早期 puts（直写 MMIO）
 * @param s NUL 字符串；NULL 忽略
 * @details 步骤：1. 空指针直接返回；2. 逐字符 early_putc。
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
