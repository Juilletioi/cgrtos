/**
 * @file trace.c
 * @brief 简易内核 Trace 环形缓冲（ISR 安全写入）
 * @author Cong Zhou / Juilletioi
 * @version 5.1.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 固定记录结构写入静态环；仅关本地 IRQ，不取 g_klock。
 * 溢出采用覆盖最旧记录并置 OVERFLOW 标志。
 */
#include "cgrtos.h"

#if CONFIG_USE_TRACE

#if (CONFIG_TRACE_ENTRIES & (CONFIG_TRACE_ENTRIES - 1)) != 0
#error "CONFIG_TRACE_ENTRIES must be power of 2"
#endif

typedef struct {
    uint32_t ts;     /**< mtime 低 32 位 */
    uint16_t type;   /**< cgrtos_trace_type_t */
    uint8_t  cpu;
    uint8_t  flags;  /**< bit0=overflow since last dump */
    uint32_t a0;
    uint32_t a1;
} cgrtos_trace_rec_t;

static cgrtos_trace_rec_t g_trace[CONFIG_TRACE_ENTRIES];
static volatile uint32_t  g_trace_head; /* next write index (monotonic) */
static volatile uint32_t  g_trace_drop; /* overwritten count */

/**
 * @brief 写入一条 Trace 记录
 * @details 关本地 IRQ → 取 head++ → 填槽 → 恢复 IRQ。
 * @param[in] type 事件类型
 * @param[in] a0   参数 0
 * @param[in] a1   参数 1
 * @return 无
 * @retval 无
 * @note ISR 安全；禁用时为空操作宏
 * @warning 高频事件可能覆盖旧记录
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_trace_event(uint16_t type, uint32_t a0, uint32_t a1)
{
    uint64_t flags = cgrtos_irq_save();
    uint32_t idx = g_trace_head++;
    uint32_t slot = idx & (CONFIG_TRACE_ENTRIES - 1U);
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_trace_rec_t *r = &g_trace[slot];

    if (idx >= CONFIG_TRACE_ENTRIES) {
        g_trace_drop++;
        r->flags = 1;
    } else {
        r->flags = 0;
    }
    r->ts = (uint32_t)cgrtos_mtime_read();
    r->type = type;
    r->cpu = cpu;
    r->a0 = a0;
    r->a1 = a1;
    cgrtos_irq_restore(flags);
}

/**
 * @brief 清空 Trace 缓冲
 * @details 重置 head/drop；不擦除槽内容。
 * @return 无
 * @retval 无
 * @note 诊断用
 * @warning 与并发写入存在竞态（可接受）
 * @attention ❌ ISR 慎用；❌ 不阻塞
 */
void cgrtos_trace_reset(void)
{
    uint64_t flags = cgrtos_irq_save();
    g_trace_head = 0;
    g_trace_drop = 0;
    cgrtos_irq_restore(flags);
}

/**
 * @brief 导出最近记录到数组（从旧到新）
 * @details 拷贝最多 max 条；返回实际条数。
 * @param[out] out 输出缓冲；每条 4 个 uint32：ts,type|cpu<<16,a0,a1
 * @param[in]  max 最大条数
 * @return 实际导出条数
 * @retval >=0 条数
 * @note out 布局紧凑便于 CLI/测试
 * @warning out 须可写
 * @attention ❌ ISR 慎用；❌ 不阻塞
 */
uint32_t cgrtos_trace_export(uint32_t *out, uint32_t max)
{
    uint64_t flags;
    uint32_t head, n, start, i;

    if (!out || max == 0) {
        return 0;
    }
    flags = cgrtos_irq_save();
    head = g_trace_head;
    n = head < CONFIG_TRACE_ENTRIES ? head : CONFIG_TRACE_ENTRIES;
    if (n > max) {
        n = max;
    }
    start = (head >= n) ? (head - n) : 0;
    for (i = 0; i < n; i++) {
        uint32_t slot = (start + i) & (CONFIG_TRACE_ENTRIES - 1U);
        cgrtos_trace_rec_t *r = &g_trace[slot];
        out[i * 4 + 0] = r->ts;
        out[i * 4 + 1] = (uint32_t)r->type | ((uint32_t)r->cpu << 16) |
                         ((uint32_t)r->flags << 24);
        out[i * 4 + 2] = r->a0;
        out[i * 4 + 3] = r->a1;
    }
    cgrtos_irq_restore(flags);
    return n;
}

/**
 * @brief 打印 Trace 摘要到 UART
 * @details 导出最近至多 32 条并 printf。
 * @return 无
 * @retval 无
 * @note 调试命令
 * @warning UART 可能阻塞
 * @attention ❌ ISR；✅ 可能阻塞
 */
void cgrtos_trace_dump(void)
{
    uint32_t buf[32 * 4];
    uint32_t n = cgrtos_trace_export(buf, 32);
    cgrtos_printf("trace: entries=%u drop~=%u (showing %u)\n",
                  (unsigned)(g_trace_head < CONFIG_TRACE_ENTRIES ?
                                 g_trace_head : CONFIG_TRACE_ENTRIES),
                  (unsigned)g_trace_drop, (unsigned)n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t meta = buf[i * 4 + 1];
        cgrtos_printf("  ts=%u cpu=%u type=%u a0=%u a1=%u%s\n",
                      (unsigned)buf[i * 4 + 0],
                      (unsigned)((meta >> 16) & 0xFF),
                      (unsigned)(meta & 0xFFFF),
                      (unsigned)buf[i * 4 + 2],
                      (unsigned)buf[i * 4 + 3],
                      (meta & (1U << 24)) ? " ovf" : "");
    }
}

#else /* !CONFIG_USE_TRACE */

void cgrtos_trace_event(uint16_t type, uint32_t a0, uint32_t a1)
{
    (void)type;
    (void)a0;
    (void)a1;
}
void cgrtos_trace_reset(void) {}
uint32_t cgrtos_trace_export(uint32_t *out, uint32_t max)
{
    (void)out;
    (void)max;
    return 0;
}
void cgrtos_trace_dump(void) {}

#endif /* CONFIG_USE_TRACE */
