/**
 * @file uart_ns16550.c
 * @brief NS16550A UART 驱动（QEMU riscv virt）
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
 * ## 硬件要点（NS16550A）
 * - LSR bit5 = THRE（发送保持寄存器空）；bit0 = DR（接收数据就绪）。
 * - 驱动对 '\\n' 自动补 '\\r'，方便串口终端。
 * - 编译期要求定义 HAL_BOARD_UART_KIND_NS16550。
 */
#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

#ifndef HAL_BOARD_UART_KIND_NS16550
#error "uart_ns16550.c requires HAL_BOARD_UART_KIND_NS16550"
#endif

/** @brief UART MMIO 基址（板级 HAL_BOARD_UART_BASE） */
#define UART_BASE HAL_BOARD_UART_BASE

/**
 * @brief 取得 NS16550 寄存器的字节指针
 * @details
 * 1. 以 UART_BASE + off 计算 MMIO 地址。
 * 2. 返回 volatile uint8_t*，供后续读写。
 * @param[in] off 相对基址的字节偏移（如 RBR/THR/LSR）
 * @return 对应寄存器的 volatile 指针
 * @retval 非 NULL 有效 MMIO 指针
 * @note 内联辅助；偏移须与板级 HAL_BOARD_UART_* 宏一致
 * @warning 非法 off 会导致未定义的 MMIO 访问
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline volatile uint8_t *uart_reg(unsigned off)
{
    return (volatile uint8_t *)(UART_BASE + off);
}

/**
 * @brief NS16550 硬件初始化：关中断 / 8N1 / 开 FIFO
 * @details
 * 1. IER=0 关闭 UART 中断。
 * 2. LCR=0x03：8 数据位、无校验、1 停止位。
 * 3. FCR=0x01 使能 FIFO；MCR=0。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_console_init 经 ops->init 调用；QEMU 可忽略波特率
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t uart_hw_init(hal_device_t *dev)
{
    (void)dev;
    *uart_reg(HAL_BOARD_UART_IER) = 0;
    *uart_reg(HAL_BOARD_UART_LCR) = 0x03;
    *uart_reg(HAL_BOARD_UART_FCR) = 0x01;
    *uart_reg(HAL_BOARD_UART_MCR) = 0x00;
    return HAL_OK;
}

/**
 * @brief 阻塞发送一字符到 NS16550
 * @details
 * 1. 轮询 LSR.THRE 直至发送保持寄存器空。
 * 2. 写入 THR。
 * 3. 若为 '\\n'，再次等待 THRE 并追加 '\\r'（CRLF）。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] c   待发送字符
 * @return 无
 * @retval 无
 * @note 由 HAL 在控制台锁内调用；本函数不加锁
 * @warning 轮询等待 THRE，可能长时间占用 CPU
 * @attention ✅ ISR-safe；✅ 阻塞（轮询 TX THRE）
 * @internal
 */
static void uart_hw_putc(hal_device_t *dev, char c)
{
    (void)dev;
    while ((*uart_reg(HAL_BOARD_UART_LSR) & 0x20U) == 0) {
    }
    *uart_reg(HAL_BOARD_UART_THR) = (uint8_t)c;
    if (c == '\n') {
        while ((*uart_reg(HAL_BOARD_UART_LSR) & 0x20U) == 0) {
        }
        *uart_reg(HAL_BOARD_UART_THR) = (uint8_t)'\r';
    }
}

/**
 * @brief 非阻塞接收一字符
 * @details
 * 1. 若 LSR.DR 未置位，返回 -1。
 * 2. 否则读 RBR 并返回。
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
    if ((*uart_reg(HAL_BOARD_UART_LSR) & 0x01U) == 0) {
        return -1;
    }
    return (int)(*uart_reg(HAL_BOARD_UART_RBR));
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
 * @brief 向 HAL 导出 NS16550 控制台设备描述符
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
 * @brief 极早期 / trap 诊断用单字符输出（直写 MMIO，不经 HAL）
 * @details
 * 1. 委托 uart_hw_putc 直写 NS16550 MMIO。
 * 2. '\\n' 自动补 '\\r'。
 * @param[in] c 待输出字符
 * @return 无
 * @retval 无
 * @note 仅供异常诊断等底层路径；应用请用 hal_console_*
 * @warning 无锁、轮询阻塞；多核并发输出可能字节交错
 * @attention ✅ ISR-safe；✅ 阻塞（轮询 TX THRE）
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
 * @retval 无
 * @note 仅供异常诊断等底层路径；应用请用 hal_console_puts
 * @warning 无锁、轮询阻塞；多核并发输出可能字节交错
 * @attention ✅ ISR-safe；✅ 阻塞（轮询 TX THRE）
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
