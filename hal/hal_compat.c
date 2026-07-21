/**
 * @file hal_compat.c
 * @brief 历史 cgrtos_* 外设 API → HAL 用户 API 薄封装
 *
 * @details
 * ## 分层位置
 * @code
 *   应用 / 内核 / 测试
 *         |  cgrtos_uart_* / cgrtos_plic_* / ...
 *         v
 *   本文件（兼容层）
 *         |  直接转调 hal_*
 *         v
 *   HAL 用户 API（hal_core.c）
 *         |
 *         v
 *   板级 Driver（arch/riscv）
 * @endcode
 *
 * - 本文件属于 HAL 侧，**允许**调用 `hal_*`。
 * - 板级驱动 **禁止** 再实现这些符号，也禁止反向调用本文件。
 * - `pdPASS`/`pdFAIL` 与 `HAL_OK`/`HAL_ERR_*` 在此做转换。
 *
 * ## 移植注意
 * 换板后只要 `hal_board_init` 注册了对应类别设备，本兼容层无需改。
 */

#include "hal.h"
#include "../kernel/cgrtos.h"

/* -------------------------------------------------------------------------- */
/* CPU                                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief 体系结构早期初始化（兼容）
 *
 * @details 步骤：
 * 1. 调用 `hal_cpu_init()`（打开 mie 中 MSIE|MTIE|MEIE）。
 * 2. 忽略返回值（历史 API 为 void；失败时后续外设 init 会暴露）。
 */
void cgrtos_arch_init(void)
{
    (void)hal_cpu_init();
}

/* -------------------------------------------------------------------------- */
/* Console                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化 UART 控制台（兼容）
 * @details 步骤：1. 转调 `hal_console_init()`；2. 忽略状态码（历史 void）。
 */
void cgrtos_uart_init(void)
{
    (void)hal_console_init();
}

/**
 * @brief 输出一字符（兼容）
 * @param c 字符；驱动侧对 '\\n' 会补 '\\r'
 * @details 步骤：1. 转调 `hal_console_putc`（内部持控制台锁）。
 */
void cgrtos_uart_putc(char c)
{
    hal_console_putc(c);
}

/**
 * @brief 阻塞读一字符（兼容）
 * @return 收到的字符
 * @details 步骤：1. 转调 `hal_console_getc`（空闲可 yield，不长期占锁）。
 */
char cgrtos_uart_getc(void)
{
    return hal_console_getc();
}

/**
 * @brief 非阻塞读一字符（兼容）
 * @return 0..255 数据；-1 表示 RX 空
 * @details 步骤：1. 转调 `hal_console_pollc`；2. 原样返回（-1 非 HAL 错误码）。
 */
int cgrtos_uart_pollc(void)
{
    return hal_console_pollc();
}

/**
 * @brief 输出 NUL 字符串（兼容）
 * @param s 字符串；NULL 安全由 HAL 处理
 * @details 步骤：1. 转调 `hal_console_puts`（整串持锁）。
 */
void cgrtos_uart_puts(const char *s)
{
    hal_console_puts(s);
}

/* -------------------------------------------------------------------------- */
/* Timer                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化本核系统定时器（兼容）
 * @param rate 期望 tick Hz；0 时 HAL 使用 CONFIG_TICK_RATE_HZ
 * @details 步骤：1. 转调 `hal_timer_init((uint32_t)rate)`。
 */
void cgrtos_clint_init(tick_t rate)
{
    (void)hal_timer_init((uint32_t)rate);
}

/**
 * @brief 读 mtime（兼容）
 * @return 64 位自由运行计数
 * @details 步骤：1. 转调 `hal_mtime_read()`。
 */
uint64_t cgrtos_mtime_read(void)
{
    return hal_mtime_read();
}

/* -------------------------------------------------------------------------- */
/* IRQC / PLIC                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化本 hart PLIC（兼容）
 * @details 步骤：1. 转调 `hal_irqc_init()`（内含首次 `cgrtos_irq_init`）。
 */
void cgrtos_plic_init(void)
{
    (void)hal_irqc_init();
}

/**
 * @brief PLIC claim（兼容）
 * @return 中断源 ID；0=无
 * @details 步骤：1. 转调 `hal_irqc_claim()`。
 * @note 应用/测试可用；trap 入口应直调驱动，不经本函数。
 */
uint32_t cgrtos_plic_claim(void)
{
    return hal_irqc_claim();
}

/**
 * @brief PLIC complete（兼容）
 * @param irq 先前 claim 的源号
 * @details 步骤：1. 转调 `hal_irqc_complete(irq)`。
 */
void cgrtos_plic_complete(uint32_t irq)
{
    hal_irqc_complete(irq);
}

/**
 * @brief 设置本 hart PLIC threshold（兼容）
 * @param threshold 新阈值
 * @details 步骤：1. 转调 `hal_irqc_set_threshold(threshold)`。
 */
void cgrtos_plic_set_threshold(uint32_t threshold)
{
    hal_irqc_set_threshold(threshold);
}

/**
 * @brief 读本 hart PLIC threshold（兼容）
 * @return 当前阈值
 * @details 步骤：1. 转调 `hal_irqc_get_threshold()`。
 */
uint32_t cgrtos_plic_get_threshold(void)
{
    return hal_irqc_get_threshold();
}

/**
 * @brief 设置中断源优先级（兼容）
 * @param irq      源编号
 * @param priority 优先级
 * @return pdPASS / pdFAIL
 * @details 步骤：
 * 1. 调用 `hal_irqc_set_priority`。
 * 2. HAL_OK → pdPASS，否则 pdFAIL。
 */
int cgrtos_plic_set_priority(uint32_t irq, uint32_t priority)
{
    return (hal_irqc_set_priority(irq, priority) == HAL_OK) ? pdPASS : pdFAIL;
}

/**
 * @brief 读中断源优先级（兼容）
 * @param irq 源编号
 * @return 优先级；非法为 0
 * @details 步骤：1. 转调 `hal_irqc_get_priority(irq)`。
 */
uint32_t cgrtos_plic_get_priority(uint32_t irq)
{
    return hal_irqc_get_priority(irq);
}

/**
 * @brief 对本 hart 使能中断源（兼容）
 * @param irq 源编号
 * @return pdPASS / pdFAIL
 * @details 步骤：1. `hal_irqc_enable`；2. 映射状态码。
 */
int cgrtos_plic_enable(uint32_t irq)
{
    return (hal_irqc_enable(irq) == HAL_OK) ? pdPASS : pdFAIL;
}

/**
 * @brief 对本 hart 禁用中断源（兼容）
 * @param irq 源编号
 * @return pdPASS / pdFAIL
 * @details 步骤：1. `hal_irqc_disable`；2. 映射状态码。
 */
int cgrtos_plic_disable(uint32_t irq)
{
    return (hal_irqc_disable(irq) == HAL_OK) ? pdPASS : pdFAIL;
}

/* -------------------------------------------------------------------------- */
/* IPI                                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief 向目标核发送软件 IPI（兼容）
 * @param core 目标 hart id
 * @details 步骤：
 * 1. 调用 `hal_ipi_send(core)`。
 * 2. 忽略返回值（历史 API 为 void；非法 hart 时为空操作）。
 */
void cgrtos_smp_send_ipi(uint8_t core)
{
    (void)hal_ipi_send(core);
}
