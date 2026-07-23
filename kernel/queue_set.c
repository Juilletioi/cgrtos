/**
 * @file queue_set.c
 * @brief QueueSet：在多个 Queue / Semaphore / StreamBuffer 上阻塞 select
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * QueueSet 实现类似 POSIX select 的多路复用：
 *
 * - **成员表** `members[]`：登记 Queue、Semaphore、StreamBuffer 及其类型；
 *   添加成员时在 IPC 对象上反向挂 `qset` 指针，便于 poke 时找到所属集合。
 * - **就绪环** `ready[]`：FIFO 环形队列，存储就绪成员的索引；同一对象不会重复入队。
 * - **等待队列** `wait_q`：`cgrtos_queue_set_select` 阻塞时挂入，成员就绪时 poke 唤醒。
 *
 * IPC 路径在资源可用时调用 cgrtos_queue_set_poke；select 从就绪环 pop 成员指针。
 *
 * @see cgrtos_qset
 */
#include "cgrtos.h"
#include <string.h>

static cgrtos_queue_set_t g_qsets[CGRTOS_MAX_QUEUE_SET];

/**
 * @brief 在 members[] 中按对象指针查找成员索引
 * @details 线性扫描 members[0..n_members)，比较 obj 指针；匹配则返回索引 i。
 * @param[in] set QueueSet 指针
 * @param[in] obj 成员对象指针（Queue/Sem/Stream）
 * @return 成员索引；未找到返回 -1
 * @retval >=0 成员索引
 * @retval -1  未注册
 * @note O(n) 线性查找
 * @warning 无
 * @attention ❌ ISR；❌ block
 * @internal
 */
static int qs_find_member(cgrtos_queue_set_t *set, void *obj)
{
    for (uint32_t i = 0; i < set->n_members; i++) {
        if (set->members[i].obj == obj) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief 检查就绪环中是否已包含指定对象
 * @details 从 rq_tail 起遍历 rq_cnt 个就绪槽；通过 ready[i] 索引取 members[idx].obj 与 obj 比较。
 * @param[in] set QueueSet 指针
 * @param[in] obj 待查对象指针
 * @return 1 已存在；0 不存在
 * @retval 1 就绪环中已有该对象
 * @retval 0 不在就绪环中
 * @note 用于 push 前去重
 * @warning 无
 * @attention ❌ ISR；❌ block
 * @internal
 */
static int qs_ready_contains(cgrtos_queue_set_t *set, void *obj)
{
    uint32_t i = set->rq_tail;
    for (uint32_t n = 0; n < set->rq_cnt; n++) {
        uint8_t idx = set->ready[i];
        if (idx < set->n_members && set->members[idx].obj == obj) {
            return 1;
        }
        i = (i + 1U) % CGRTOS_QUEUE_SET_LENGTH;
    }
    return 0;
}

/**
 * @brief 将成员推入就绪环（去重、容量检查）
 * @details 查找 obj 在 members 中的索引；非成员或已就绪则返回；环满则丢弃；否则写入 ready[rq_head] 并推进 head/rq_cnt。
 * @param[in] set QueueSet 指针
 * @param[in] obj 就绪的成员对象指针
 * @return 无
 * @retval 无
 * @note 环满时静默丢弃 poke
 * @warning 调用方通常已在临界区内
 * @attention ✅ ISR；❌ block
 * @internal
 */
static void qs_push_ready(cgrtos_queue_set_t *set, void *obj)
{
    int mi = qs_find_member(set, obj);
    if (mi < 0) {
        return;
    }
    if (qs_ready_contains(set, obj)) {
        return;
    }
    if (set->rq_cnt >= CGRTOS_QUEUE_SET_LENGTH) {
        return;
    }
    set->ready[set->rq_head] = (uint8_t)mi;
    set->rq_head = (set->rq_head + 1U) % CGRTOS_QUEUE_SET_LENGTH;
    set->rq_cnt++;
}

/**
 * @brief 从就绪环弹出一个成员对象指针
 * @details 若 rq_cnt==0 返回 NULL；从 ready[rq_tail] 取成员索引，推进 tail 并 rq_cnt--；校验索引后返回 members[idx].obj。
 * @param[in] set QueueSet 指针
 * @return 成员 obj 指针；环空或索引非法时返回 NULL
 * @retval 非 NULL 就绪成员对象指针
 * @retval NULL     就绪环为空或索引损坏
 * @note FIFO 顺序
 * @warning 无
 * @attention ❌ ISR；❌ block
 * @internal
 */
static void *qs_pop_ready(cgrtos_queue_set_t *set)
{
    if (set->rq_cnt == 0) {
        return 0;
    }
    uint8_t idx = set->ready[set->rq_tail];
    set->rq_tail = (set->rq_tail + 1U) % CGRTOS_QUEUE_SET_LENGTH;
    set->rq_cnt--;
    if (idx >= set->n_members) {
        return 0;
    }
    return set->members[idx].obj;
}

/**
 * @brief 创建 QueueSet 实例
 * @details 忽略 length（固定环长由宏定义）；临界区内在静态池 g_qsets 中找 in_use==0 的槽，清零并置 in_use=1。
 * @param[in] length 参数保留（实际容量固定为 CGRTOS_QUEUE_SET_LENGTH）
 * @return 成功返回实例指针；池满返回 NULL
 * @retval 非 NULL 新创建的 QueueSet
 * @retval NULL     静态池已满
 * @note length 参数当前未使用
 * @warning 无
 * @attention ❌ ISR；❌ block
 */
cgrtos_queue_set_t *cgrtos_queue_set_create(uint32_t length)
{
    (void)length; /* fixed CGRTOS_QUEUE_SET_LENGTH */
    cgrtos_enter_critical();
    for (uint32_t i = 0; i < CGRTOS_MAX_QUEUE_SET; i++) {
        if (!g_qsets[i].in_use) {
            memset(&g_qsets[i], 0, sizeof(g_qsets[i]));
            g_qsets[i].in_use = 1;
            cgrtos_exit_critical();
            return &g_qsets[i];
        }
    }
    cgrtos_exit_critical();
    return 0;
}

/**
 * @brief 向 QueueSet 添加成员（内部通用实现）
 * @details 校验 set/obj/in_use；检查成员数未达上限且 obj 未重复；登记 type/obj 并设置 IPC 对象 qset 反向指针；若成员已有资源则 push 就绪环。
 * @param[in] set  QueueSet 指针
 * @param[in] type 成员类型（QUEUE/SEM/STREAM）
 * @param[in] obj  成员对象指针
 * @return pdPASS 成功；pdFAIL 失败
 * @retval pdPASS 成员已添加
 * @retval pdFAIL 参数非法、已满或重复注册
 * @note 添加时若 cnt/count/avail>0 立即标记就绪
 * @warning 同一 obj 不可加入多个 QueueSet
 * @attention ❌ ISR；❌ block
 * @internal
 */
static int qs_add(cgrtos_queue_set_t *set, cgrtos_qset_type_t type, void *obj)
{
    if (!set || !obj || !set->in_use) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    if (set->n_members >= CGRTOS_QUEUE_SET_LENGTH || qs_find_member(set, obj) >= 0) {
        cgrtos_exit_critical();
        return pdFAIL;
    }
    /* 3. 登记新成员 */
    uint32_t i = set->n_members++;
    set->members[i].type = type;
    set->members[i].obj = obj;
    /* 4. 设置反向指针并按当前状态决定是否就绪 */
    if (type == CGRTOS_QSET_QUEUE) {
        ((cgrtos_queue_t *)obj)->qset = set;
        if (((cgrtos_queue_t *)obj)->cnt > 0) {
            qs_push_ready(set, obj);
        }
    } else if (type == CGRTOS_QSET_SEM) {
        ((cgrtos_sem_t *)obj)->qset = set;
        if (((cgrtos_sem_t *)obj)->count > 0) {
            qs_push_ready(set, obj);
        }
    } else if (type == CGRTOS_QSET_STREAM) {
        ((cgrtos_stream_buffer_t *)obj)->qset = set;
        if (((cgrtos_stream_buffer_t *)obj)->avail > 0) {
            qs_push_ready(set, obj);
        }
    }
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 向 QueueSet 添加 Queue 成员
 * @details 调用 qs_add，类型为 CGRTOS_QSET_QUEUE。
 * @param[in] set QueueSet 指针
 * @param[in] q   Queue 指针
 * @return pdPASS 成功；pdFAIL 失败
 * @retval pdPASS 成员已添加
 * @retval pdFAIL 添加失败
 * @note 便捷包装
 * @warning 无
 * @attention ❌ ISR；❌ block
 */
int cgrtos_queue_set_add_queue(cgrtos_queue_set_t *set, cgrtos_queue_t *q)
{
    return qs_add(set, CGRTOS_QSET_QUEUE, q);
}

/**
 * @brief 向 QueueSet 添加 Semaphore 成员
 * @details 调用 qs_add，类型为 CGRTOS_QSET_SEM。
 * @param[in] set QueueSet 指针
 * @param[in] sem Semaphore 指针
 * @return pdPASS 成功；pdFAIL 失败
 * @retval pdPASS 成员已添加
 * @retval pdFAIL 添加失败
 * @note 便捷包装
 * @warning 无
 * @attention ❌ ISR；❌ block
 */
int cgrtos_queue_set_add_sem(cgrtos_queue_set_t *set, cgrtos_sem_t *sem)
{
    return qs_add(set, CGRTOS_QSET_SEM, sem);
}

/**
 * @brief 向 QueueSet 添加 StreamBuffer 成员
 * @details 调用 qs_add，类型为 CGRTOS_QSET_STREAM。
 * @param[in] set QueueSet 指针
 * @param[in] sb  StreamBuffer 指针
 * @return pdPASS 成功；pdFAIL 失败
 * @retval pdPASS 成员已添加
 * @retval pdFAIL 添加失败
 * @note 便捷包装
 * @warning 无
 * @attention ❌ ISR；❌ block
 */
int cgrtos_queue_set_add_stream(cgrtos_queue_set_t *set, cgrtos_stream_buffer_t *sb)
{
    return qs_add(set, CGRTOS_QSET_STREAM, sb);
}

/**
 * @brief 从 QueueSet 移除成员
 * @details 临界区内查找成员索引；按类型清除 IPC 对象 qset 反向指针；压缩 members 数组；清空就绪环（索引可能已失效）。
 * @param[in] set QueueSet 指针
 * @param[in] obj 待移除成员对象指针
 * @return pdPASS 成功；pdFAIL 失败
 * @retval pdPASS 成员已移除
 * @retval pdFAIL 参数非法或未找到
 * @note 移除后清空整个就绪环
 * @warning 不清除 IPC 对象内已有数据
 * @attention ❌ ISR；❌ block
 */
int cgrtos_queue_set_remove(cgrtos_queue_set_t *set, void *obj)
{
    if (!set || !obj) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    int mi = qs_find_member(set, obj);
    if (mi < 0) {
        cgrtos_exit_critical();
        return pdFAIL;
    }
    /* 3. 清除反向指针 */
    cgrtos_qset_type_t type = set->members[mi].type;
    if (type == CGRTOS_QSET_QUEUE) {
        ((cgrtos_queue_t *)obj)->qset = 0;
    } else if (type == CGRTOS_QSET_SEM) {
        ((cgrtos_sem_t *)obj)->qset = 0;
    } else if (type == CGRTOS_QSET_STREAM) {
        ((cgrtos_stream_buffer_t *)obj)->qset = 0;
    }
    /* 4. 压缩成员表 */
    for (uint32_t i = (uint32_t)mi; i + 1 < set->n_members; i++) {
        set->members[i] = set->members[i + 1];
    }
    set->n_members--;
    /* 5. 就绪环索引失效，整体清空 */
    set->rq_head = set->rq_tail = set->rq_cnt = 0;
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief IPC 路径通知 QueueSet 某成员已就绪
 * @details 校验 set/obj/in_use；将 obj 推入就绪环（去重）；若 wait_q 有 select 等待者则弹出最高优先级者并 unblock。
 * @param[in] set QueueSet 指针
 * @param[in] obj 就绪的成员对象指针
 * @return 无
 * @retval 无
 * @note 调用方通常已在 IPC 临界区内（嵌套安全）
 * @warning 环满时 poke 被静默丢弃
 * @attention ✅ ISR；❌ block
 */
void cgrtos_queue_set_poke(cgrtos_queue_set_t *set, void *obj)
{
    if (!set || !obj || !set->in_use) {
        return;
    }
    /* 2. 推入就绪环 */
    qs_push_ready(set, obj);
    /* 3. 唤醒 select 等待者 */
    if (set->wait_q) {
        cgrtos_task_t *w = cgrtos_wait_list_pop_highest(&set->wait_q);
        if (w) {
            w->wake_ok = 1;
            cgrtos_sched_unblock(w);
        }
    }
}

/**
 * @brief 阻塞等待 QueueSet 中任一成员就绪
 * @details 循环：临界区尝试 qs_pop_ready；有则返回 obj；timeout==0 或无 running 任务则返回 NULL；否则阻塞到 wait_q 并 yield。
 * @param[in] set     QueueSet 指针
 * @param[in] timeout 阻塞超时 tick；0 表示不阻塞
 * @return 就绪成员 obj 指针；超时或无就绪返回 NULL
 * @retval 非 NULL 就绪成员指针
 * @retval NULL     超时、非阻塞无就绪或 idle 无法阻塞
 * @note 返回指针后调用方须对该对象 take/recv
 * @warning 就绪环 pop 后该成员事件被消费
 * @attention ❌ ISR；✅ block
 */
void *cgrtos_queue_set_select(cgrtos_queue_set_t *set, tick_t timeout)
{
    if (!set || !set->in_use) {
        return 0;
    }

    for (;;) {
        cgrtos_enter_critical();
        uint8_t cpu = arch_cpu_id();
        cgrtos_task_t *cur = g_current[cpu];
        /* 2. 尝试弹出就绪成员 */
        void *obj = qs_pop_ready(set);
        if (obj) {
            cgrtos_exit_critical();
            return obj;
        }
        /* 3. 非阻塞且无就绪 */
        if (timeout == 0) {
            cgrtos_exit_critical();
            return 0;
        }
        /* 4. idle 任务无法阻塞 */
        if (!cur || cur->id == 0) {
            cgrtos_exit_critical();
            return 0;
        }
        /* 5. 阻塞并挂入 wait_q */
        cgrtos_sched_block(cur, BLOCK_QUEUE_SET, set, timeout);
        cgrtos_wait_list_add(&set->wait_q, cur);
        cgrtos_exit_critical();
        cgrtos_sched_yield();
        if (!cur->wake_ok) {
            return 0;
        }
        timeout = 0;
    }
}

/**
 * @brief 删除 QueueSet 并释放槽位
 * @details 临界区内遍历所有成员清除 qset 反向指针；唤醒所有 wait_q 等待者（wake_ok=0）；清零 set 结构；yield 后返回。
 * @param[in] set QueueSet 指针
 * @return pdPASS 成功；pdFAIL 失败
 * @retval pdPASS 已删除
 * @retval pdFAIL 参数无效
 * @note 被唤醒 select 者 wake_ok=0 表示取消
 * @warning 不删除成员 IPC 对象本身
 * @attention ❌ ISR；✅ block
 */
int cgrtos_queue_set_delete(cgrtos_queue_set_t *set)
{
    if (!set || !set->in_use) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    /* 2. 解除所有成员的反向引用 */
    for (uint32_t i = 0; i < set->n_members; i++) {
        void *obj = set->members[i].obj;
        cgrtos_qset_type_t type = set->members[i].type;
        if (type == CGRTOS_QSET_QUEUE && obj) {
            ((cgrtos_queue_t *)obj)->qset = 0;
        } else if (type == CGRTOS_QSET_SEM && obj) {
            ((cgrtos_sem_t *)obj)->qset = 0;
        } else if (type == CGRTOS_QSET_STREAM && obj) {
            ((cgrtos_stream_buffer_t *)obj)->qset = 0;
        }
    }
    /* 3. 取消所有 select 等待者 */
    while (set->wait_q) {
        cgrtos_task_t *w = cgrtos_wait_list_pop_highest(&set->wait_q);
        if (w) {
            w->wake_ok = 0;
            cgrtos_sched_unblock(w);
        }
    }
    memset(set, 0, sizeof(*set));
    cgrtos_exit_critical();
    cgrtos_sched_yield();
    return pdPASS;
}
