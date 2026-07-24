/**
 * @file uart_pl011.c
 * @brief PL011 UART（QEMU virt @ 0x09000000）板级驱动
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * ## 分层位置
 * - 本文件 = Driver：只访问 MMIO，实现 console ops。
 * - 不调用任何 hal_* 用户 API（禁止依赖倒置）。
 * - HAL 通过 drv_uart_device() 取得本设备并注册 / 分发。
 * - 早期诊断用 drv_uart_early_putc / puts 直写 MMIO。
 *
 * ## 硬件要点（PL011）
 * - FR.TXFF / FR.RXFE 指示 FIFO 满/空。
 * - 驱动对 '\\n' 自动补 '\\r'，方便串口终端。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

/** @brief UART_DR：数据寄存器（收发共用） */
#define UART_DR   (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x00))
/** @brief UART_FR：标志寄存器（TXFF/RXFE 等） */
#define UART_FR   (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x18))
/** @brief UART_IBRD：整数波特率除数 */
#define UART_IBRD (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x24))
/** @brief UART_FBRD：小数波特率除数 */
#define UART_FBRD (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x28))
/** @brief UART_LCRH：线控制（字长 / FIFO 使能） */
#define UART_LCRH (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x2c))
/** @brief UART_CR：控制寄存器（UARTEN/TXE/RXE） @warning 改配置前须先关 UARTEN */
#define UART_CR   (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x30))
/** @brief UART_IMSC：中断屏蔽置位 */
#define UART_IMSC (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x38))
/** @brief UART_ICR：中断清除 @warning 写 1 清对应位 */
#define UART_ICR  (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x44))

/** @brief FR bit5：TX FIFO 满 */
#define FR_TXFF (1u << 5)
/** @brief FR bit4：RX FIFO 空 */
#define FR_RXFE (1u << 4)

/**
 * @brief PL011 硬件初始化：波特率 / 8N1 / 使能收发
 * @details
 * 1. CR=0 关闭 UART；ICR 清全部中断标志。
 * 2. IBRD/FBRD 设约 115200（QEMU 可忽略波特率）。
 * 3. LCRH：8N1 + FEN；IMSC=0 关中断。
 * 4. CR：UARTEN|TXE|RXE。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_console_init 经 ops-->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t uart_hw_init(hal_device_t *dev)
{
    (void)dev;
    UART_CR = 0;
    UART_ICR = 0x7ff;
    UART_IBRD = 26; /* ~115200 @ 48MHz reference (QEMU ignores baud) */
    UART_FBRD = 3;
    UART_LCRH = (3u << 5) | (1u << 4); /* 8N1 + FEN */
    UART_IMSC = 0;
    UART_CR = (1u << 0) | (1u << 8) | (1u << 9); /* UARTEN|TXE|RXE */
    return HAL_OK;
}

/**
 * @brief 阻塞发送一字符到 PL011
 * @details
 * 1. 轮询 FR.TXFF 直至 FIFO 可写。
 * 2. 写入 DR 低 8 位。
 * 3. 若为 '\\n'，再次等待可写并追加 '\\r'（CRLF）。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] c   待发送字符
 * @return 无
 * @retval 无
 * @note 由 HAL 在控制台锁内调用；本函数不加锁
 * @warning 轮询等待 TXFF 清空，可能长时间占用 CPU
 * @attention ✅ ISR-safe；✅ 阻塞（轮询 TX FIFO）
 * @internal
 */
static void uart_hw_putc(hal_device_t *dev, char c)
{
    (void)dev;
    while (UART_FR & FR_TXFF) {
    }
    UART_DR = (uint32_t)(uint8_t)c;
    if (c == '\n') {
        while (UART_FR & FR_TXFF) {
        }
        UART_DR = '\r';
    }
}

/**
 * @brief 非阻塞接收一字符
 * @details
 * 1. 若 FR.RXFE 置位，返回 -1。
 * 2. 否则读 DR 低 8 位并返回。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return 接收到的字节或 -1
 * @retval 0..255 有效数据
 * @retval -1     RX 空（非 HAL 错误码）
 * @note 由 HAL hal_console_pollc 在锁内调用
 * @warning 返回值 -1 勿与 HAL_ERR_* 混淆
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static int uart_hw_pollc(hal_device_t *dev)
{
    (void)dev;
    if (UART_FR & FR_RXFE) {
        return -1;
    }
    return (int)(UART_DR & 0xffu);
}

static const hal_console_ops_t s_uart_ops = {
    .init = uart_hw_init,
    .putc = uart_hw_putc,
    .pollc = uart_hw_pollc,
};

static hal_device_t s_uart_dev = {
    .name = "uart0",
    .class = HAL_DEV_CONSOLE,
    .mmio_base = HAL_BOARD_UART_BASE,
    .ops = &s_uart_ops,
    .priv = 0,
    .flags = 0,
};

/**
 * @brief 向 HAL 导出 PL011 控制台设备描述符
 * @details
 * 1. 返回静态 s_uart_dev 指针。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 勿释放返回值；应用应使用 hal_console_*
 * @warning 驱动不得自行调用 hal_device_register
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 */
hal_device_t *drv_uart_device(void)
{
    return &s_uart_dev;
}

/**
 * @brief 极早期 / 异常诊断用单字符输出（直写 MMIO，不经 HAL）
 * @details
 * 1. 委托 uart_hw_putc 直写 PL011 MMIO。
 * 2. '\\n' 自动补 '\\r'。
 * @param[in] c 待输出字符
 * @return 无
 * @retval 无
 * @note 仅供异常诊断等底层路径；应用请用 hal_console_*
 * @warning 无锁、轮询阻塞；多核并发输出可能字节交错
 * @attention ✅ ISR-safe；✅ 阻塞（轮询 TX FIFO）
 */
void drv_uart_early_putc(char c)
{
    uart_hw_putc(&s_uart_dev, c);
}

/**
 * @brief 极早期 / 异常诊断用字符串输出（直写 MMIO，不经 HAL）
 * @details
 * 1. s 为 NULL 时直接返回。
 * 2. 逐字符调用 drv_uart_early_putc 输出。
 * @param[in] s NUL 结尾字符串；NULL 忽略
 * @return 无
 * @retval 无
 * @note 仅供异常诊断等底层路径；应用请用 hal_console_puts
 * @warning 无锁、轮询阻塞；多核并发输出可能字节交错
 * @attention ✅ ISR-safe；✅ 阻塞（轮询 TX FIFO）
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
