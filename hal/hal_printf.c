/**
 * @file hal_printf.c
 * @brief 简易格式化输出（HAL 侧实现，持控制台锁）
 *
 * @details
 * ## 分层
 * - 属于 HAL / 用户可见输出路径，调用 `hal_console_lock` +
 *   `hal_console_putc_unlocked`，**不**直接碰 UART MMIO。
 * - 板级驱动不得实现本符号，也不得为打印去调本函数形成环。
 *
 * ## 锁策略
 * 1. `cgrtos_printf` 入口取得控制台锁（关本核中断 + 自旋锁）。
 * 2. 整条消息格式化期间持锁，保证 SMP 下不会半行交错。
 * 3. 锁不是 `g_klock`，避免嵌在 `cgrtos_enter_critical` 内死锁。
 * 4. 持锁期间禁止 yield / 再取内核大锁。
 *
 * ## 支持的格式
 * `%s` `%d` `%i` `%u` `%x` `%c` `%p` `%%`，以及 `%lu` `%ld` `%lx` `%ls`。
 */

#include "hal.h"
#include "../kernel/cgrtos.h"

/**
 * @brief 已持锁时输出一字符
 * @param c 字符
 * @details 步骤：1. 转调 `hal_console_putc_unlocked(c)`。
 */
static void pchar(char c)
{
    hal_console_putc_unlocked(c);
}

/**
 * @brief 已持锁时输出 NUL 字符串
 * @param s 字符串；NULL 打印 "(null)"
 * @details 步骤：1. 空指针替换；2. 逐字符 pchar 直至 '\\0'。
 */
static void pstr(const char *s)
{
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        pchar(*s++);
    }
}

/**
 * @brief 已持锁时输出无符号数
 * @param v   数值
 * @param hex 非 0 为十六进制，否则十进制
 * @details 步骤：
 * 1. v==0 → 输出 '0' 返回。
 * 2. 循环取余/取半字节，数字逆序写入 buf。
 * 3. 逆序吐出 buf，得到正确位序。
 */
static void pnum(unsigned long v, int hex)
{
    char buf[20];
    int i = 0;

    if (v == 0) {
        pchar('0');
        return;
    }
    while (v && i < 20) {
        int d = hex ? (int)(v & 0xFU) : (int)(v % 10UL);
        buf[i++] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
        if (hex) {
            v >>= 4;
        } else {
            v /= 10UL;
        }
    }
    while (i > 0) {
        pchar(buf[--i]);
    }
}

/**
 * @brief 已持锁时输出有符号十进制
 * @param v 有符号值
 * @details 步骤：1. 负则先 '-' 再取反；2. pnum 十进制。
 */
static void psnum(long v)
{
    if (v < 0) {
        pchar('-');
        v = -v;
    }
    pnum((unsigned long)v, 0);
}

/**
 * @brief 简易格式化打印
 * @param fmt 格式串
 * @param ... 可变参数
 *
 * @details 步骤：
 * 1. `hal_console_lock()` 取得整消息输出权。
 * 2. `va_start` 初始化参数列表。
 * 3. 逐字符扫描 fmt：
 *    a. 非 '%' → 直接 pchar；
 *    b. '%' 后解析格式符并从 ap 取参格式化。
 * 4. `va_end` 清理。
 * 5. `hal_console_unlock()` 释放锁。
 */
void cgrtos_printf(const char *fmt, ...)
{
    __builtin_va_list ap;

    if (!fmt) {
        return;
    }

    hal_console_lock();
    __builtin_va_start(ap, fmt);

    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            pchar(*f);
            continue;
        }
        f++;
        if (*f == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            pstr(s);
        } else if (*f == 'd' || *f == 'i') {
            psnum(__builtin_va_arg(ap, int));
        } else if (*f == 'u') {
            pnum(__builtin_va_arg(ap, unsigned int), 0);
        } else if (*f == 'x') {
            pnum(__builtin_va_arg(ap, unsigned int), 1);
        } else if (*f == 'c') {
            pchar((char)__builtin_va_arg(ap, int));
        } else if (*f == 'p') {
            pstr("0x");
            pnum((unsigned long)(uintptr_t)__builtin_va_arg(ap, void *), 1);
        } else if (*f == '%') {
            pchar('%');
        } else if (*f == 'l') {
            f++;
            if (*f == 'u') {
                pnum(__builtin_va_arg(ap, unsigned long), 0);
            } else if (*f == 'd') {
                psnum(__builtin_va_arg(ap, long));
            } else if (*f == 'x') {
                pnum(__builtin_va_arg(ap, unsigned long), 1);
            } else if (*f == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                pstr(s);
            }
        }
    }

    __builtin_va_end(ap);
    hal_console_unlock();
}
