/**
 * @file uart_ns16550.c
 * @brief NS16550A UART 驱动（QEMU riscv virt）
 * @author Cong Zhou / Juilletioi
 * @version 5.2.0
 * @date 2026-07-23
 * @copyright CG-RTOS
 */
#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

#ifndef HAL_BOARD_UART_KIND_NS16550
#error "uart_ns16550.c requires HAL_BOARD_UART_KIND_NS16550"
#endif

#define UART_BASE HAL_BOARD_UART_BASE

static inline volatile uint8_t *uart_reg(unsigned off)
{
    return (volatile uint8_t *)(UART_BASE + off);
}

static hal_status_t uart_hw_init(hal_device_t *dev)
{
    (void)dev;
    *uart_reg(HAL_BOARD_UART_IER) = 0;
    *uart_reg(HAL_BOARD_UART_LCR) = 0x03;
    *uart_reg(HAL_BOARD_UART_FCR) = 0x01;
    *uart_reg(HAL_BOARD_UART_MCR) = 0x00;
    return HAL_OK;
}

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

hal_device_t *drv_uart_device(void)
{
    return &s_uart_dev;
}

void drv_uart_early_putc(char c)
{
    uart_hw_putc(&s_uart_dev, c);
}

void drv_uart_early_puts(const char *s)
{
    if (!s) {
        return;
    }
    while (*s) {
        drv_uart_early_putc(*s++);
    }
}
