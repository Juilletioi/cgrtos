/**
 * @file hal_printf.c
 * @brief 简易格式化输出（HAL 侧实现，持控制台锁）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
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
 * @details 转调 hal_console_putc_unlocked(c) 将字符写入控制台。
 * @param[in] c 待输出字符
 * @return 无
 * @retval 无
 * @note 调用方须已持有 hal_console_lock
 * @warning 无
 * @attention ❌ ISR；❌ block
 * @internal
 */
static void pchar(char c)
{
    hal_console_putc_unlocked(c);
}

/**
 * @brief 已持锁时输出 NUL 结尾字符串
 * @details NULL 指针替换为 "(null)"；逐字符 pchar 直至 '\\0'。
 * @param[in] s 字符串；NULL 打印 "(null)"
 * @return 无
 * @retval 无
 * @note 调用方须已持有 hal_console_lock
 * @warning 字符串须以 NUL 结尾
 * @attention ❌ ISR；❌ block
 * @internal
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
 * @details v==0 直接输出 '0'；否则循环取余/取半字节逆序写入 buf 再逆序输出。
 * @param[in] v   数值
 * @param[in] hex 非 0 为十六进制，否则十进制
 * @return 无
 * @retval 无
 * @note 内部 buf 固定 20 字节
 * @warning 无
 * @attention ❌ ISR；❌ block
 * @internal
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
 * @details 负值先输出 '-' 再对绝对值调用 pnum 十进制输出。
 * @param[in] v 有符号值
 * @return 无
 * @retval 无
 * @note 调用方须已持有 hal_console_lock
 * @warning 无
 * @attention ❌ ISR；❌ block
 * @internal
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
 * @details hal_console_lock 取得输出权；va_start 后逐字符扫描 fmt，解析 '%' 格式符并从 ap 取参格式化；va_end 后 hal_console_unlock。
 * @param[in] fmt 格式串
 * @param[in] ... 可变参数
 * @return 无
 * @retval 无
 * @note 支持 %s %d %i %u %x %c %p %% 及 %lu %ld %lx %ls
 * @warning fmt 为 NULL 时直接返回；持锁期间禁止 yield
 * @attention ❌ ISR；❌ block
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
