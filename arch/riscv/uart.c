/**
 * @file uart.c
 * @brief Nuclei evalsoc UART 驱动与简易 printf 实现。
 *
 * 提供字符收发、格式化输出及 puts 接口，供内核调试与日志使用。
 */

#include "../../kernel/cgrtos.h"
#include <stdarg.h>

/** @brief UART 外设基址。 */
#define UART_BASE 0x10013000
/** @brief 发送数据寄存器偏移。 */
#define UART_TXDATA 0x00
/** @brief 接收数据寄存器偏移。 */
#define UART_RXDATA 0x04
/** @brief 发送控制寄存器偏移。 */
#define UART_TXCTRL 0x08
/** @brief 接收控制寄存器偏移。 */
#define UART_RXCTRL 0x0C

/**
 * @brief 初始化 UART 收发通道。
 *
 * @details
 * 1. 获取 TXCTRL 与 RXCTRL 寄存器指针。
 * 2. 分别置最低位，使能发送与接收通道。
 */
void cgrtos_uart_init(void) {
    /* 1. 获取 TX/RX 控制寄存器指针 */
    volatile uint32_t* txctrl = (volatile uint32_t*)(UART_BASE + UART_TXCTRL);
    volatile uint32_t* rxctrl = (volatile uint32_t*)(UART_BASE + UART_RXCTRL);
    /* 2. 置最低位使能收发通道 */
    *txctrl |= 1;
    *rxctrl |= 1;
}

/**
 * @brief 发送单个字符。
 *
 * @param c 待发送字符；遇到换行符 '\n' 时会自动追加回车 '\r'。
 *
 * @details
 * 1. 轮询 TXDATA 寄存器 bit 31（TXFULL），直至发送 FIFO 可写。
 * 2. 将字符低 8 位写入 TXDATA 寄存器。
 * 3. 若字符为 '\n'，再次等待可写后追加 '\r'，实现 CRLF 换行。
 */
void cgrtos_uart_putc(char c) {
    volatile uint32_t* txdata = (volatile uint32_t*)(UART_BASE + UART_TXDATA);
    /* 1. 等待 TX FIFO 非满 */
    while (*txdata & 0x80000000U) {}
    /* 2. 写入字符低 8 位 */
    *txdata = (unsigned char)c;
    if (c == '\n') {
        /* 3. 换行时追加回车实现 CRLF */
        while (*txdata & 0x80000000U) {}
        *txdata = (unsigned char)'\r';
    }
}

/**
 * @brief 非阻塞接收单个字符。
 *
 * @return 0..255 为接收到的数据字节；-1 表示接收 FIFO 为空。
 *
 * @details
 * 1. 读取 RXDATA 寄存器。
 * 2. 若 bit 31（RXEMPTY）置位，返回 -1 表示无数据。
 * 3. 否则返回低 8 位有效数据。
 */
int cgrtos_uart_pollc(void)
{
    /* 1. 读取 RXDATA 寄存器 */
    volatile uint32_t *rxdata = (volatile uint32_t *)(UART_BASE + UART_RXDATA);
    uint32_t v = *rxdata;
    /* 2. RXEMPTY 置位则返回 -1 */
    if (v & 0x80000000U) {
        return -1;
    }
    /* 3. 返回低 8 位有效数据 */
    return (int)(v & 0xFFU);
}

/**
 * @brief 阻塞接收单个字符。
 *
 * @return 接收到的字符（低 8 位有效）。
 *
 * @details
 * 1. 循环调用 cgrtos_uart_pollc 尝试非阻塞读取。
 * 2. 若 FIFO 为空且调度器已运行，调用 cgrtos_task_yield 让出 CPU，避免忙等饿死同核任务。
 * 3. 读到有效字符后返回。
 */
char cgrtos_uart_getc(void)
{
    int c;
    /* 1-2. 轮询读取，空 FIFO 且调度器运行则 yield */
    while ((c = cgrtos_uart_pollc()) < 0) {
        if (g_sched_run) {
            cgrtos_task_yield();
        }
    }
    /* 3. 返回有效字符 */
    return (char)c;
}

/**
 * @brief printf 内部：输出单字符。
 *
 * @param c 字符。
 *
 * @details
 * 1. 委托 cgrtos_uart_putc 完成实际硬件发送。
 */
static void pchar(char c) {
    /* 1. 委托硬件发送 */
    cgrtos_uart_putc(c);
}

/**
 * @brief printf 内部：输出以 NUL 结尾的字符串。
 *
 * @param s 字符串指针。
 *
 * @details
 * 1. 逐字符遍历字符串直至 '\0'。
 * 2. 每个字符通过 pchar 输出。
 */
static void pstr(const char* s) {
    /* 1-2. 逐字符输出直至 NUL */
    while (*s) pchar(*s++);
}

/**
 * @brief printf 内部：无符号数转字符串输出。
 *
 * @param v   数值。
 * @param hex 非零则十六进制，否则十进制。
 *
 * @details
 * 1. 若 v 为 0，直接输出 '0' 并返回。
 * 2. 循环取余/取位，将各位数字存入 buf（逆序）。
 * 3. 从 buf 逆序输出，得到正确位序的字符串。
 */
static void pnum(unsigned long v, int hex) {
    char buf[20]; int i = 0;
    /* 1. 零值直接输出 '0' */
    if (v == 0) { pchar('0'); return; }
    /* 2. 取余/取位逆序存入 buf */
    while (v && i < 20) { int d = hex ? (v & 0xF) : (v % 10); buf[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10); if (hex) v >>= 4; else v /= 10; }
    /* 3. 逆序输出 buf 得到正确位序 */
    while (i > 0) pchar(buf[--i]);
}

/**
 * @brief printf 内部：有符号数输出。
 *
 * @param v 有符号数值。
 *
 * @details
 * 1. 若 v 为负，先输出 '-' 并取绝对值。
 * 2. 委托 pnum 以十进制输出无符号部分。
 */
static void psnum(long v) {
    /* 1. 负数先输出 '-' 并取绝对值 */
    if (v < 0) { pchar('-'); v = -v; }
    /* 2. 十进制输出无符号部分 */
    pnum((unsigned long)v, 0);
}

/**
 * @brief 简易格式化输出（支持 %s %d %i %u %x %c %p %% 及 %l 修饰符）。
 *
 * @param fmt 格式字符串。
 * @param ... 可变参数。
 *
 * @details
 * 1. 使用 va_start 初始化可变参数列表。
 * 2. 逐字符扫描 fmt：
 *    a. 非 '%' 字符直接输出；
 *    b. 遇 '%' 解析下一格式符，从 ap 取参并格式化输出。
 * 3. 支持的格式：%s %d %i %u %x %c %p %%，以及 %lu %ld %lx %ls。
 * 4. va_end 清理参数列表。
 */
void cgrtos_printf(const char* fmt, ...) {
    /* 1. 初始化可变参数列表 */
    va_list ap; va_start(ap, fmt);
    /* 2. 逐字符扫描 fmt 并格式化输出 */
    for (const char* f = fmt; *f; f++) {
        if (*f != '%') { pchar(*f); continue; } f++;
        if (*f == 's') { const char* s = va_arg(ap, const char*); pstr(s ? s : "(null)"); }
        else if (*f == 'd' || *f == 'i') { psnum(va_arg(ap, int)); }
        else if (*f == 'u') { pnum(va_arg(ap, unsigned int), 0); }
        else if (*f == 'x') { pnum(va_arg(ap, unsigned int), 1); }
        else if (*f == 'c') { pchar((char)va_arg(ap, int)); }
        else if (*f == 'p') { pstr("0x"); pnum((unsigned long)(uintptr_t)va_arg(ap, void*), 1); }
        else if (*f == '%') { pchar('%'); }
        else if (*f == 'l') { f++;
            if (*f == 'u') { pnum(va_arg(ap, unsigned long), 0); }
            else if (*f == 'd') { psnum(va_arg(ap, long)); }
            else if (*f == 'x') { pnum(va_arg(ap, unsigned long), 1); }
            else if (*f == 's') { const char* s = va_arg(ap, const char*); pstr(s ? s : "(null)"); }
        }
    }
    /* 4. 清理参数列表 */
    va_end(ap);
}

/**
 * @brief 输出以 NUL 结尾的字符串。
 *
 * @param s 字符串指针。
 *
 * @details
 * 1. 委托 pstr 逐字符输出直至 '\0'。
 */
void cgrtos_uart_puts(const char* s) {
    /* 1. 委托 pstr 逐字符输出 */
    pstr(s);
}
