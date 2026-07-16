/**
 * @file queue_set.c
 * @brief QueueSet：在多个 Queue / Semaphore / StreamBuffer 上阻塞 select
 *
 * ## 模块设计
 *
 * QueueSet 实现类似 POSIX select 的多路复用：
 *
 * - **成员表** `members[]`：登记 Queue、Semaphore、StreamBuffer 及其类型；
 *   添加成员时在 IPC 对象上反向挂 `qset` 指针，便于 poke 时找到所属集合。
 * - **就绪环** `ready[]`：FIFO 环形队列，存储就绪成员的索引；同一对象不会重复入队。
 * - **等待队列** `wait_q`：`cgrtos_queue_set_select` 阻塞时挂入，成员就绪时 poke 唤醒。
 *
 * ## 工作流程
 *
 * 1. IPC 路径（send/give/写入 stream）在资源可用时调用 `cgrtos_queue_set_poke`。
 * 2. poke 将成员索引 push 到就绪环，并唤醒 select 等待者。
 * 3. select 从就绪环 pop 一个成员指针，调用方再对该对象 take/recv。
 *
 * @see cgrtos_qset
 */
#include "cgrtos.h"
#include <string.h>

static cgrtos_queue_set_t g_qsets[CGRTOS_MAX_QUEUE_SET];

/**
 * @brief 在 members[] 中按对象指针查找成员索引
 *
 * @param set QueueSet 指针
 * @param obj 成员对象指针（Queue/Sem/Stream）
 * @return 成员索引；未找到返回 -1
 *
 * @details
 * 1. 线性扫描 members[0..n_members)，比较 obj 指针。
 * 2. 匹配则返回索引 i；扫描结束未找到返回 -1。
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
 *
 * @param set QueueSet 指针
 * @param obj 待查对象指针
 * @return 1 已存在；0 不存在
 *
 * @details
 * 1. 从 rq_tail 起遍历 rq_cnt 个就绪槽。
 * 2. 通过 ready[i] 索引取 members[idx].obj 与 obj 比较。
 * 3. 任一匹配则返回 1；遍历完返回 0。
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
 *
 * @param set QueueSet 指针
 * @param obj 就绪的成员对象指针
 *
 * @details
 * 1. 查找 obj 在 members 中的索引；非成员则直接返回。
 * 2. 若就绪环已含该 obj，去重返回。
 * 3. 若 rq_cnt 已达 CGRTOS_QUEUE_SET_LENGTH，丢弃（环满）。
 * 4. 将成员索引写入 ready[rq_head]，推进 head 并 rq_cnt++。
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
 *
 * @param set QueueSet 指针
 * @return 成员 obj 指针；环空或索引非法时返回 NULL
 *
 * @details
 * 1. 若 rq_cnt==0，返回 NULL。
 * 2. 从 ready[rq_tail] 取成员索引，推进 tail 并 rq_cnt--。
 * 3. 校验索引 < n_members，返回 members[idx].obj；否则返回 NULL。
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
 *
 * @param length 参数保留（实际容量固定为 CGRTOS_QUEUE_SET_LENGTH）
 * @return 成功返回实例指针；池满返回 NULL
 *
 * @details
 * 1. 忽略 length（固定环长由宏定义）。
 * 2. 进入临界区，在静态池 g_qsets 中找 in_use==0 的槽。
 * 3. 清零并置 in_use=1，退出临界区返回指针。
 * 4. 池满则返回 NULL。
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
 *
 * @param set  QueueSet 指针
 * @param type 成员类型（QUEUE/SEM/STREAM）
 * @param obj  成员对象指针
 * @return pdPASS 成功；pdFAIL 失败
 *
 * @details
 * 1. 校验 set/obj/in_use。
 * 2. 临界区内检查：成员数未达上限且 obj 未重复注册。
 * 3. 在 members[n_members] 记录 type 与 obj，n_members++。
 * 4. 按类型在 IPC 对象上设置 qset 反向指针。
 * 5. 若成员当前已有资源（cnt/count/avail>0），立即 push 到就绪环。
 * 6. 退出临界区返回 pdPASS。
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
 *
 * @param set QueueSet 指针
 * @param q   Queue 指针
 * @return pdPASS 成功；pdFAIL 失败
 *
 * @details
 * 1. 调用 qs_add，类型为 CGRTOS_QSET_QUEUE。
 */
int cgrtos_queue_set_add_queue(cgrtos_queue_set_t *set, cgrtos_queue_t *q)
{
    return qs_add(set, CGRTOS_QSET_QUEUE, q);
}

/**
 * @brief 向 QueueSet 添加 Semaphore 成员
 *
 * @param set QueueSet 指针
 * @param sem Semaphore 指针
 * @return pdPASS 成功；pdFAIL 失败
 *
 * @details
 * 1. 调用 qs_add，类型为 CGRTOS_QSET_SEM。
 */
int cgrtos_queue_set_add_sem(cgrtos_queue_set_t *set, cgrtos_sem_t *sem)
{
    return qs_add(set, CGRTOS_QSET_SEM, sem);
}

/**
 * @brief 向 QueueSet 添加 StreamBuffer 成员
 *
 * @param set QueueSet 指针
 * @param sb  StreamBuffer 指针
 * @return pdPASS 成功；pdFAIL 失败
 *
 * @details
 * 1. 调用 qs_add，类型为 CGRTOS_QSET_STREAM。
 */
int cgrtos_queue_set_add_stream(cgrtos_queue_set_t *set, cgrtos_stream_buffer_t *sb)
{
    return qs_add(set, CGRTOS_QSET_STREAM, sb);
}

/**
 * @brief 从 QueueSet 移除成员
 *
 * @param set QueueSet 指针
 * @param obj 待移除成员对象指针
 * @return pdPASS 成功；pdFAIL 失败
 *
 * @details
 * 1. 校验 set/obj；临界区内查找成员索引 mi。
 * 2. 未找到则返回 pdFAIL。
 * 3. 按类型清除 IPC 对象上的 qset 反向指针。
 * 4. 压缩 members 数组（mi 之后的条目前移），n_members--。
 * 5. 清空就绪环（索引可能已失效）。
 * 6. 退出临界区返回 pdPASS。
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
 *
 * @param set QueueSet 指针
 * @param obj 就绪的成员对象指针
 *
 * @details
 * 1. 校验 set/obj/in_use；调用方通常已在临界区内（嵌套安全）。
 * 2. 将 obj 推入就绪环（qs_push_ready 去重）。
 * 3. 若 wait_q 有 select 等待者，弹出最高优先级者，wake_ok=1 并 unblock。
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
 *
 * @param set     QueueSet 指针
 * @param timeout 阻塞超时 tick；0 表示不阻塞
 * @return 就绪成员 obj 指针；超时或无就绪返回 NULL
 *
 * @details
 * 1. 校验 set/in_use。
 * 2. 循环：进入临界区，尝试 qs_pop_ready；有则立即返回 obj。
 * 3. timeout==0 且无就绪，返回 NULL。
 * 4. 当前任务无效（idle）则返回 NULL。
 * 5. 阻塞当前任务（BLOCK_QUEUE_SET），挂入 wait_q，yield。
 * 6. 被唤醒后再试（timeout 置 0）；wake_ok==0 表示超时，返回 NULL。
 */
void *cgrtos_queue_set_select(cgrtos_queue_set_t *set, tick_t timeout)
{
    if (!set || !set->in_use) {
        return 0;
    }

    for (;;) {
        cgrtos_enter_critical();
        uint8_t cpu = (uint8_t)read_csr(mhartid);
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
 *
 * @param set QueueSet 指针
 * @return pdPASS 成功；pdFAIL 失败
 *
 * @details
 * 1. 校验 set/in_use。
 * 2. 临界区内遍历所有成员，清除 IPC 对象上的 qset 反向指针。
 * 3. 唤醒所有 wait_q 等待者（wake_ok=0）。
 * 4. 清零整个 set 结构，退出临界区。
 * 5. yield 后返回 pdPASS。
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
