/**
 * @file klog.c
 * @brief 内核分级日志（模块 6）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 */

#include "cgrtos.h"

#if CONFIG_USE_KLOG

/** @brief 运行时日志级别阈值（低于等于此级别的消息才会输出） */
static int g_log_runtime_level = CONFIG_LOG_LEVEL;

/**
 * @brief 设置运行时日志级别
 * @details 将 level 钳制到 [CGRTOS_LOG_NONE, CGRTOS_LOG_DEBUG] 并写入全局阈值。不阻塞、不切换；可在任务上下文调用。
 * @param[in] level 新的日志级别
 * @return 无
 * @retval 无
 * @note 仅影响后续 cgrtos_log 输出，不改变编译期 CONFIG_LOG_LEVEL
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_log_set_level(int level)
{
    if (level < CGRTOS_LOG_NONE) {
        level = CGRTOS_LOG_NONE;
    }
    if (level > CGRTOS_LOG_DEBUG) {
        level = CGRTOS_LOG_DEBUG;
    }
    g_log_runtime_level = level;
}

/**
 * @brief 获取当前运行时日志级别
 * @details 返回 g_log_runtime_level。只读，不阻塞、不切换。
 * @return 当前日志级别
 * @retval CGRTOS_LOG_NONE..CGRTOS_LOG_DEBUG 有效级别值
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_log_get_level(void)
{
    return g_log_runtime_level;
}

/**
 * @brief 输出分级日志消息
 * @details 若 level 高于运行时阈值则直接返回；否则格式化 [级别][标签] 消息并通过 cgrtos_printf 输出。printf 可能阻塞 UART。
 * @param[in] level 消息级别（ERROR/WARN/INFO/DEBUG）
 * @param[in] tag   模块标签；NULL 时使用 "k"
 * @param[in] msg   消息正文；NULL 时使用空串
 * @return 无
 * @retval 无
 * @note 被过滤的消息不产生任何输出
 * @warning 在 ISR 中调用可能因 UART 阻塞而延长中断延迟
 * @attention ❌ ISR；✅ block/switch
 */
void cgrtos_log(int level, const char *tag, const char *msg)
{
    const char *lvl;
    if (level > g_log_runtime_level || level <= CGRTOS_LOG_NONE) {
        return;
    }
    switch (level) {
    case CGRTOS_LOG_ERROR: lvl = "E"; break;
    case CGRTOS_LOG_WARN:  lvl = "W"; break;
    case CGRTOS_LOG_INFO:  lvl = "I"; break;
    default:               lvl = "D"; break;
    }
    /* 分级日志带 tick，便于与 Trace 对照 */
    cgrtos_printf("[%s][%s][t%lu] %s\n", lvl, tag ? tag : "k",
                  (unsigned long)cgrtos_get_ticks(), msg ? msg : "");
}

#else

/**
 * @brief 设置运行时日志级别（桩：KLOG 未启用）
 * @details CONFIG_USE_KLOG 关闭时的空实现，忽略 level。不阻塞、不切换。
 * @param[in] level 被忽略的日志级别
 * @return 无
 * @retval 无
 * @note KLOG 未编译进内核
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_log_set_level(int level) { (void)level; }

/**
 * @brief 获取当前运行时日志级别（桩：KLOG 未启用）
 * @details 始终返回 CGRTOS_LOG_NONE，表示无日志输出。不阻塞、不切换。
 * @return CGRTOS_LOG_NONE
 * @retval CGRTOS_LOG_NONE 日志功能未启用
 * @note KLOG 未编译进内核
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_log_get_level(void) { return CGRTOS_LOG_NONE; }

/**
 * @brief 输出分级日志消息（桩：KLOG 未启用）
 * @details 忽略所有参数，不产生输出。不阻塞、不切换。
 * @param[in] level 被忽略的级别
 * @param[in] tag   被忽略的标签
 * @param[in] msg   被忽略的消息
 * @return 无
 * @retval 无
 * @note KLOG 未编译进内核
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_log(int level, const char *tag, const char *msg)
{
    (void)level;
    (void)tag;
    (void)msg;
}

#endif
