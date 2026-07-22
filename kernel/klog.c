/**
 * @file klog.c
 * @brief 内核分级日志（模块6）
 */
#include "cgrtos.h"

#if CONFIG_USE_KLOG

static int g_log_runtime_level = CONFIG_LOG_LEVEL;

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

int cgrtos_log_get_level(void)
{
    return g_log_runtime_level;
}

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
    cgrtos_printf("[%s][%s] %s\n", lvl, tag ? tag : "k", msg ? msg : "");
}

#else

void cgrtos_log_set_level(int level) { (void)level; }
int cgrtos_log_get_level(void) { return CGRTOS_LOG_NONE; }
void cgrtos_log(int level, const char *tag, const char *msg)
{
    (void)level;
    (void)tag;
    (void)msg;
}

#endif
