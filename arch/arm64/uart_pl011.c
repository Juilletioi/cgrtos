/**
 * @file uart_pl011.c
 * @brief PL011 UART（QEMU virt @ 0x09000000）
 */
#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

#define UART_DR   (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x00))
#define UART_FR   (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x18))
#define UART_IBRD (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x24))
#define UART_FBRD (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x28))
#define UART_LCRH (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x2c))
#define UART_CR   (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x30))
#define UART_IMSC (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x38))
#define UART_ICR  (*(volatile uint32_t *)(HAL_BOARD_UART_BASE + 0x44))

#define FR_TXFF (1u << 5)
#define FR_RXFE (1u << 4)

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
