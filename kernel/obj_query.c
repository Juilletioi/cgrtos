/**
 * @file obj_query.c
 * @brief 系统对象查询：池占用统计与列表导出
 * @author Cong Zhou / Juilletioi
 * @version 5.1.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 */
#include "cgrtos.h"
#include <string.h>

#if CONFIG_USE_OBJ_QUERY

extern cgrtos_sem_t         g_sems[CGRTOS_MAX_SEM];
extern uint32_t             g_sem_cnt;
extern cgrtos_mutex_t       g_mtxs[CGRTOS_MAX_MUTEX];
extern cgrtos_queue_t       g_qs[CGRTOS_MAX_QUEUE];
extern uint32_t             g_q_cnt;
extern cgrtos_event_group_t g_egs[CGRTOS_MAX_EVENT];

/**
 * @brief 统计等待链长度
 * @details 沿 next 指针遍历等待队列，上限为 CONFIG_MAX_TASKS 以防环。
 * @param[in] head 等待队列头
 * @return 节点数
 * @retval >=0 长度
 * @note 仅在临界区内调用
 * @warning 调用方须已持临界区
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static uint32_t waitq_len(cgrtos_task_t *volatile head)
{
    uint32_t n = 0;
    cgrtos_task_t *t = (cgrtos_task_t *)head;
    while (t && n < CONFIG_MAX_TASKS) {
        n++;
        t = t->next;
    }
    return n;
}

/**
 * @brief 填充对象池占用快照
 * @details 扫描各静态池，统计 used/capacity。
 * @param[out] out 输出；不可为 NULL
 * @return 0 成功；-1 参数非法
 * @retval 0  成功
 * @retval -1 out 为空
 * @note 短临界区
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_objects_stats_get(cgrtos_objects_stats_t *out)
{
    uint32_t i, n;
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    cgrtos_enter_critical();

    n = 0;
    for (i = 0; i < CONFIG_MAX_TASKS; i++) {
        if (g_tasks[i].id != 0 && g_tasks[i].state != TASK_DELETED) {
            n++;
        }
    }
    out->tasks_used = n;
    out->tasks_max = CONFIG_MAX_TASKS;

    n = 0;
    for (i = 0; i < CGRTOS_MAX_SEM; i++) {
        if (g_sems[i].max != 0) {
            n++;
        }
    }
    out->sem_used = n;
    out->sem_max = CGRTOS_MAX_SEM;

    n = 0;
    for (i = 0; i < CGRTOS_MAX_MUTEX; i++) {
        if (g_mtxs[i].in_use) {
            n++;
        }
    }
    out->mutex_used = n;
    out->mutex_max = CGRTOS_MAX_MUTEX;

    n = 0;
    for (i = 0; i < CGRTOS_MAX_QUEUE; i++) {
        if (g_qs[i].buf) {
            n++;
        }
    }
    out->queue_used = n;
    out->queue_max = CGRTOS_MAX_QUEUE;

    n = 0;
    for (i = 0; i < CGRTOS_MAX_EVENT; i++) {
        if (g_egs[i].in_use) {
            n++;
        }
    }
    out->event_used = n;
    out->event_max = CGRTOS_MAX_EVENT;

    out->timer_used = cgrtos_timer_count_used();
    out->timer_max = CGRTOS_MAX_TIMER;

    cgrtos_exit_critical();
    return 0;
}

/**
 * @brief 导出信号量对象摘要
 * @details 填充 handle/count/max/waiters。
 * @param[out] out 输出数组；NULL 仅计数
 * @param[in]  max 容量
 * @return 写入条数（或总数）
 * @retval >=0 条数
 * @note 无
 * @warning 截断时仅填 max 条
 * @attention ❌ ISR；❌ block/switch
 */
uint32_t cgrtos_sem_list_export(cgrtos_sem_info_t *out, uint32_t max)
{
    uint32_t n = 0, i;
    cgrtos_enter_critical();
    for (i = 0; i < CGRTOS_MAX_SEM; i++) {
        if (g_sems[i].max == 0) {
            continue;
        }
        if (out && n < max) {
            out[n].handle = &g_sems[i];
            out[n].count = g_sems[i].count;
            out[n].max = g_sems[i].max;
            out[n].waiters = waitq_len(g_sems[i].wait_q);
        }
        n++;
    }
    cgrtos_exit_critical();
    return n;
}

/**
 * @brief 导出互斥量对象摘要
 * @details 扫描互斥量池，填充 handle/owner_id/recursive/waiters。
 * @param[out] out 输出数组；NULL 仅计数
 * @param[in]  max 容量
 * @return 条数
 * @retval >=0 条数
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
uint32_t cgrtos_mutex_list_export(cgrtos_mutex_info_t *out, uint32_t max)
{
    uint32_t n = 0, i;
    cgrtos_enter_critical();
    for (i = 0; i < CGRTOS_MAX_MUTEX; i++) {
        if (!g_mtxs[i].in_use) {
            continue;
        }
        if (out && n < max) {
            out[n].handle = &g_mtxs[i];
            out[n].owner_id = g_mtxs[i].owner ? g_mtxs[i].owner->id : 0;
            out[n].recursive = g_mtxs[i].recursive;
            out[n].waiters = waitq_len(g_mtxs[i].wait_q);
        }
        n++;
    }
    cgrtos_exit_critical();
    return n;
}

/**
 * @brief 导出队列对象摘要
 * @details 扫描队列池，填充 handle/len/item_sz/count 与收发等待数。
 * @param[out] out 输出数组；NULL 仅计数
 * @param[in]  max 容量
 * @return 条数
 * @retval >=0 条数
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
uint32_t cgrtos_queue_list_export(cgrtos_queue_info_t *out, uint32_t max)
{
    uint32_t n = 0, i;
    cgrtos_enter_critical();
    for (i = 0; i < CGRTOS_MAX_QUEUE; i++) {
        if (!g_qs[i].buf) {
            continue;
        }
        if (out && n < max) {
            out[n].handle = &g_qs[i];
            out[n].len = g_qs[i].len;
            out[n].item_sz = g_qs[i].item_sz;
            out[n].count = g_qs[i].cnt;
            out[n].wait_send = waitq_len(g_qs[i].send_wait_q);
            out[n].wait_recv = waitq_len(g_qs[i].recv_wait_q);
        }
        n++;
    }
    cgrtos_exit_critical();
    return n;
}

/**
 * @brief 打印对象池占用到 UART
 * @details 调用 stats_get 后 printf。
 * @return 无
 * @retval 无
 * @note CLI/调试
 * @warning UART 可能阻塞
 * @attention ❌ ISR；✅ block/switch
 */
void cgrtos_objects_dump(void)
{
    cgrtos_objects_stats_t st;
    if (cgrtos_objects_stats_get(&st) != 0) {
        return;
    }
    cgrtos_printf("objects: task %u/%u sem %u/%u mtx %u/%u q %u/%u eg %u/%u tmr %u/%u\n",
                  (unsigned)st.tasks_used, (unsigned)st.tasks_max,
                  (unsigned)st.sem_used, (unsigned)st.sem_max,
                  (unsigned)st.mutex_used, (unsigned)st.mutex_max,
                  (unsigned)st.queue_used, (unsigned)st.queue_max,
                  (unsigned)st.event_used, (unsigned)st.event_max,
                  (unsigned)st.timer_used, (unsigned)st.timer_max);
}

#else /* !CONFIG_USE_OBJ_QUERY */

/**
 * @brief 填充对象池占用快照（功能未启用时的空实现）
 * @details 若 out 非空则清零并返回 -1，表示 OBJ_QUERY 未编译。
 * @param[out] out 输出缓冲；可为 NULL
 * @return 始终 -1
 * @retval -1 CONFIG_USE_OBJ_QUERY 关闭
 * @note 链接占位，保持 API 可用
 * @warning 无有效统计
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_objects_stats_get(cgrtos_objects_stats_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return -1;
}

/**
 * @brief 导出信号量摘要（功能未启用时的空实现）
 * @details 忽略参数，返回 0。
 * @param[out] out 未使用
 * @param[in]  max 未使用
 * @return 0
 * @retval 0 无对象
 * @note CONFIG_USE_OBJ_QUERY 关闭
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
uint32_t cgrtos_sem_list_export(cgrtos_sem_info_t *out, uint32_t max)
{
    (void)out;
    (void)max;
    return 0;
}

/**
 * @brief 导出互斥量摘要（功能未启用时的空实现）
 * @details 忽略参数，返回 0。
 * @param[out] out 未使用
 * @param[in]  max 未使用
 * @return 0
 * @retval 0 无对象
 * @note CONFIG_USE_OBJ_QUERY 关闭
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
uint32_t cgrtos_mutex_list_export(cgrtos_mutex_info_t *out, uint32_t max)
{
    (void)out;
    (void)max;
    return 0;
}

/**
 * @brief 导出队列摘要（功能未启用时的空实现）
 * @details 忽略参数，返回 0。
 * @param[out] out 未使用
 * @param[in]  max 未使用
 * @return 0
 * @retval 0 无对象
 * @note CONFIG_USE_OBJ_QUERY 关闭
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
uint32_t cgrtos_queue_list_export(cgrtos_queue_info_t *out, uint32_t max)
{
    (void)out;
    (void)max;
    return 0;
}

/**
 * @brief 打印对象池占用（功能未启用时的空实现）
 * @details 无操作。
 * @return 无
 * @retval 无
 * @note CONFIG_USE_OBJ_QUERY 关闭
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_objects_dump(void) {}

#endif /* CONFIG_USE_OBJ_QUERY */
