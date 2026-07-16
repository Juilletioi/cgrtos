/**
 * @file ipc.c
 * @brief 信号量、互斥量、消息队列与事件组 IPC 原语实现
 * 
 * ## 模块设计
 * 
 * 本模块实现 CGRtOS 四类经典同步/通信对象，均基于优先级等待队列与调度器阻塞机制：
 * 
 * - **信号量（Semaphore）**：计数型同步对象，`count` 表示可用令牌数；`take` 减计数或阻塞，
 *   `give` 优先唤醒最高优先级等待者，否则递增计数（不超过 `max`）。
 * - **互斥量（Mutex）**：带所有权与递归计数的互斥锁，默认启用优先级继承（PI）：
 *   高优先级任务阻塞时临时提升持有者优先级，释放后恢复 `base_prio`。
 * - **消息队列（Queue）**：定长环形缓冲，每项固定 `item_sz` 字节；独立维护
 *   `send_wait_q` / `recv_wait_q`，满/空时分别阻塞发送者与接收者。
 * - **事件组（Event Group）**：32 位标志位集合，支持 AND/OR 等待模式及退出时清除选项。
 * 
 * ## 公共基础设施
 * 
 * - 全局静态池 `g_sems[]`、`g_mtxs[]`、`g_qs[]`、`g_egs[]` 管理实例生命周期。
 * - `block_current_on_waitq` 统一封装「调度阻塞 + 挂入等待队列」。
 * - `wait_result` 在 yield 返回后检查 `wake_ok` 判定阻塞是否成功。
 * - 各对象可挂接 QueueSet（`qset` 字段），状态变化时调用 `cgrtos_queue_set_poke`。
 * - 所有可变操作在 `cgrtos_enter_critical` / `cgrtos_exit_critical` 临界区内完成。
 * 
 * @see cgrtos_sem, cgrtos_mutex, cgrtos_queue, cgrtos_event
 */
#include "cgrtos.h"
#include <string.h>

cgrtos_sem_t         g_sems[CGRTOS_MAX_SEM];
uint32_t             g_sem_cnt;
cgrtos_mutex_t       g_mtxs[CGRTOS_MAX_MUTEX];
uint32_t             g_mtx_cnt;
cgrtos_queue_t       g_qs[CGRTOS_MAX_QUEUE];
uint32_t             g_q_cnt;
cgrtos_event_group_t g_egs[CGRTOS_MAX_EVENT];
uint32_t             g_eg_cnt;

/**
 * @brief 将当前任务挂到指定等待队列并标记阻塞原因
 * 
 * @param wait_q  目标等待队列指针（各 IPC 对象的 wait_q 成员）
 * @param reason  阻塞原因枚举（BLOCK_SEM / BLOCK_MUTEX / BLOCK_QUEUE_* / BLOCK_EVENT）
 * @param obj     关联 IPC 对象指针，写入 task->block_obj
 * @param timeout 阻塞超时 tick 数
 * 
 * @return pdPASS 成功挂起；pdFAIL 当前无有效任务（如 idle）
 * 
 * @details
 * 1. 读取当前 hart 的 running 任务指针。
 * 2. 若任务为空或为 idle（id==0），无法阻塞，返回 pdFAIL。
 * 3. 调用调度器将该任务置为 BLOCKED 并记录 reason、关联对象与 timeout。
 * 4. 将任务按优先级插入 wait_q 等待链表。
 * 5. 返回 pdPASS，调用方需在 exit_critical 后 yield。
 */
static int block_current_on_waitq(cgrtos_task_t *volatile *wait_q,
block_reason_t reason, void *obj,
tick_t timeout)
{
    /* 1. 获取当前 CPU 与 running 任务 */
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *cur = g_current[cpu];
    if (!cur || cur->id == 0) {
        return pdFAIL;
    }

    /* 2. 标记阻塞并挂入等待队列 */
    cgrtos_sched_block(cur, reason, obj, timeout);
    cgrtos_wait_list_add(wait_q, cur);
    return pdPASS;
}

/**
 * @brief 检查任务被唤醒后的阻塞结果
 * 
 * @param task 被阻塞后重新运行的任务指针
 * 
 * @return pdPASS 唤醒成功（wake_ok==1）；pdFAIL 超时或被 delete 唤醒
 * 
 * @details
 * 1. 若 task 为空，视为失败。
 * 2. 读取 task->wake_ok 标志判定阻塞是否满足条件。
 */
static int wait_result(cgrtos_task_t *task)
{
    return (task && task->wake_ok) ? pdPASS : pdFAIL;
}

/**
 * @brief 动态创建计数信号量
 * 
 * @param init 初始计数值
 * @param max  最大计数值（令牌上限）
 * 
 * @return 成功返回信号量指针；参数非法或池满返回 NULL
 * 
 * @details
 * 1. 校验 max>0 且 0<=init<=max。
 * 2. 进入临界区，在 g_sems[] 中查找 max==0 的空槽。
 * 3. 初始化 count、max、lock、wait_q、qset 并递增 g_sem_cnt。
 * 4. 退出临界区并返回新实例指针；无空槽则返回 NULL。
 */
cgrtos_sem_t *cgrtos_sem_create(int32_t init, int32_t max)
{
    /* 1. 参数校验 */
    if (max <= 0 || init < 0 || init > max) {
        return 0;
    }

    /* 2. 在静态池中分配空槽 */
    cgrtos_enter_critical();
    for (uint32_t i = 0; i < CGRTOS_MAX_SEM; i++) {
        if (g_sems[i].max == 0) {
            g_sems[i].count = init;
            g_sems[i].max = max;
            g_sems[i].lock = 0;
            g_sems[i].wait_q = 0;
            g_sems[i].qset = 0;
            g_sem_cnt++;
            cgrtos_exit_critical();
            return &g_sems[i];
        }
    }
    cgrtos_exit_critical();
    return 0;
}

/**
 * @brief 创建二值信号量（初始 0，最大 1）
 * 
 * @return 二值信号量指针；池满返回 NULL
 * 
 * @details
 * 1. 调用 cgrtos_sem_create(0, 1) 创建初始为空的二值信号量。
 */
cgrtos_sem_t *cgrtos_sem_create_binary(void)
{
    return cgrtos_sem_create(0, 1);
}

/**
 * @brief 在调用者提供的静态存储上初始化信号量
 * 
 * @param sem  调用者分配的信号量结构体指针
 * @param init 初始计数值
 * @param max  最大计数值
 * 
 * @return 成功返回 sem；参数非法返回 NULL
 * 
 * @details
 * 1. 校验 sem 非空且 max>0、0<=init<=max。
 * 2. 清零结构体并设置 count、max。
 * 3. 返回 sem（不占用全局池）。
 */
cgrtos_sem_t *cgrtos_sem_create_static(cgrtos_sem_t *sem, int32_t init, int32_t max)
{
    /* 1. 参数校验 */
    if (!sem || max <= 0 || init < 0 || init > max) {
        return 0;
    }
    /* 2. 初始化静态实例 */
    memset(sem, 0, sizeof(*sem));
    sem->count = init;
    sem->max = max;
    return sem;
}

/**
 * @brief 获取信号量（P 操作 / take）
 * 
 * @param sem     信号量指针
 * @param timeout 阻塞超时 tick 数；0 表示不等待
 * 
 * @return pdPASS 成功获取；pdFAIL 参数错误或超时
 * 
 * @details
 * 1. 校验 sem 非空。
 * 2. 进入临界区，若 count>0 则递减并立即返回成功。
 * 3. 若 timeout==0 且无可用令牌，返回 pdFAIL。
 * 4. 否则将当前任务挂入 sem->wait_q 并 yield。
 * 5. 唤醒后通过 wait_result 检查 wake_ok 返回最终结果。
 */
int cgrtos_sem_take(cgrtos_sem_t *sem, tick_t timeout)
{
    /* 1. 参数校验 */
    if (!sem) {
        return pdFAIL;
    }

    /* 2. 进入临界区 */
    cgrtos_enter_critical();

    /* 3. 有可用令牌则直接递减并返回 */
    if (sem->count > 0) {
        sem->count--;
        cgrtos_exit_critical();
        return pdPASS;
    }

    /* 4. 非阻塞模式立即失败 */
    if (timeout == 0) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    /* 5. 挂入等待队列并 yield */
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *cur = g_current[cpu];
    block_current_on_waitq(&sem->wait_q, BLOCK_SEM, sem, timeout);
    cgrtos_exit_critical();

    cgrtos_sched_yield();
    return wait_result(cur);
}

/**
 * @brief 释放信号量（V 操作 / give）
 * 
 * @param sem 信号量指针
 * 
 * @return pdPASS 成功；pdFAIL 参数错误
 * 
 * @details
 * 1. 校验 sem 非空并进入临界区。
 * 2. 若有等待者，唤醒最高优先级任务并设置 wake_ok=1。
 * 3. 若挂接 QueueSet 则 poke 通知 select 等待者。
 * 4. 无等待者且 count<max 时递增 count。
 * 5. 唤醒等待者时 exit_critical 后 yield 以立即调度被唤醒任务。
 */
int cgrtos_sem_give(cgrtos_sem_t *sem)
{
    /* 1. 参数校验 */
    if (!sem) {
        return pdFAIL;
    }

    /* 2. 进入临界区 */
    cgrtos_enter_critical();

    /* 3. 有等待者则直接唤醒，否则 count++ */
    cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&sem->wait_q);
    if (waiter) {
        waiter->wake_ok = 1;
        cgrtos_sched_unblock(waiter);
        if (sem->qset) {
            cgrtos_queue_set_poke(sem->qset, sem);
        }
        cgrtos_exit_critical();
        cgrtos_sched_yield();
        return pdPASS;
    }

    if (sem->count < sem->max) {
        sem->count++;
        if (sem->qset) {
            cgrtos_queue_set_poke(sem->qset, sem);
        }
    }
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 从中断上下文释放信号量
 * 
 * @param sem 信号量指针
 * 
 * @return pdPASS 成功；pdFAIL 参数错误
 * 
 * @details
 * 1. 校验 sem 非空并进入临界区。
 * 2. 若有等待者，唤醒最高优先级任务。
 * 3. 无等待者且 count<max 时递增 count。
 * 4. 退出临界区后若唤醒了任务则调用 sched_yield_from_isr。
 */
int cgrtos_sem_give_from_isr(cgrtos_sem_t *sem)
{
    /* 1. 参数校验 */
    if (!sem) {
        return pdFAIL;
    }

    /* 2. 进入临界区 */
    cgrtos_enter_critical();

    /* 3. 有等待者则直接唤醒，否则 count++ */
    cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&sem->wait_q);
    if (waiter) {
        waiter->wake_ok = 1;
        cgrtos_sched_unblock(waiter);
        if (sem->qset) {
            cgrtos_queue_set_poke(sem->qset, sem);
        }
        cgrtos_exit_critical();
        cgrtos_sched_yield_from_isr();
        return pdPASS;
    }

    if (sem->count < sem->max) {
        sem->count++;
        if (sem->qset) {
            cgrtos_queue_set_poke(sem->qset, sem);
        }
    }
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 删除信号量并唤醒所有等待者
 * 
 * @param sem 信号量指针
 * 
 * @return pdPASS 成功；pdFAIL 参数错误
 * 
 * @details
 * 1. 校验 sem 非空并进入临界区。
 * 2. 逐个弹出 wait_q 中等待者，置 wake_ok=0 并 unblock（表示失败唤醒）。
 * 3. 清零信号量结构体并递减 g_sem_cnt。
 * 4. 退出临界区后 yield 以调度被唤醒任务。
 */
int cgrtos_sem_delete(cgrtos_sem_t *sem)
{
    /* 1. 参数校验 */
    if (!sem) {
        return pdFAIL;
    }

    /* 2. 进入临界区，唤醒所有等待者 */
    cgrtos_enter_critical();
    while (sem->wait_q) {
        cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&sem->wait_q);
        if (waiter) {
            waiter->wake_ok = 0;
            cgrtos_sched_unblock(waiter);
        }
    }
    /* 3. 释放实例 */
    memset(sem, 0, sizeof(*sem));
    if (g_sem_cnt > 0) {
        g_sem_cnt--;
    }
    cgrtos_exit_critical();
    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 动态创建互斥量
 * 
 * @return 成功返回互斥量指针；池满返回 NULL
 * 
 * @details
 * 1. 进入临界区，在 g_mtxs[] 中查找 in_use==0 的空槽。
 * 2. 清零结构体，设置 in_use=1、inherit=1（默认启用优先级继承）。
 * 3. 递增 g_mtx_cnt 并返回新实例指针。
 */
cgrtos_mutex_t *cgrtos_mutex_create(void)
{
    cgrtos_enter_critical();
    for (uint32_t i = 0; i < CGRTOS_MAX_MUTEX; i++) {
        if (!g_mtxs[i].in_use) {
            memset(&g_mtxs[i], 0, sizeof(g_mtxs[i]));
            g_mtxs[i].in_use = 1;
            g_mtxs[i].inherit = 1;
            g_mtx_cnt++;
            cgrtos_exit_critical();
            return &g_mtxs[i];
        }
    }
    cgrtos_exit_critical();
    return 0;
}

/**
 * @brief 在调用者提供的静态存储上初始化互斥量
 * 
 * @param mutex 调用者分配的互斥量结构体指针
 * 
 * @return 成功返回 mutex；参数非法返回 NULL
 * 
 * @details
 * 1. 校验 mutex 非空。
 * 2. 清零结构体，设置 in_use=1、inherit=1。
 * 3. 返回 mutex（不占用全局池）。
 */
cgrtos_mutex_t *cgrtos_mutex_create_static(cgrtos_mutex_t *mutex)
{
    if (!mutex) {
        return 0;
    }
    memset(mutex, 0, sizeof(*mutex));
    mutex->in_use = 1;
    mutex->inherit = 1;
    return mutex;
}

/**
 * @brief 对互斥量持有者应用优先级继承
 * 
 * @param mutex  互斥量指针
 * @param waiter 正在等待该互斥量的任务
 * 
 * @details
 * 1. 若未启用 inherit 或无 owner/waiter，直接返回。
 * 2. 若 waiter 优先级不高于 owner，无需提升。
 * 3. 若 owner 在 READY 队列中，先 remove 再修改 prio 后 re-add。
 * 4. 将 owner->prio 提升至 waiter->prio。
 * 5. 若 owner 正在其他核 RUNNING，发送 IPI 触发重调度。
 */
static void mutex_apply_inheritance(cgrtos_mutex_t *mutex, cgrtos_task_t *waiter)
{
    /* 1. 检查 PI 前置条件 */
    if (!mutex->inherit || !mutex->owner || !waiter) {
        return;
    }
    if (waiter->prio <= mutex->owner->prio) {
        return;
    }

    /* 2. 提升持有者优先级并更新就绪队列 */
    cgrtos_task_t *owner = mutex->owner;
    if (owner->state == TASK_READY) {
        cgrtos_sched_remove_ready(owner);
    }
    owner->prio = waiter->prio;
    if (owner->state == TASK_READY) {
        cgrtos_sched_add_ready(owner);
    } else if (owner->state == TASK_RUNNING &&
    owner->run_cpu != (uint8_t)read_csr(mhartid) &&
    owner->run_cpu < CONFIG_NUM_CORES) {
        /* 3. 跨核运行中则 IPI 通知重调度 */
        cgrtos_smp_send_ipi(owner->run_cpu);
    }
}

/**
 * @brief 根据等待队列最高优先级任务对持有者做 PI 提升
 * 
 * @param mutex 互斥量指针
 * 
 * @details
 * 1. 若未启用 inherit 或无 owner 或 wait_q 为空，直接返回。
 * 2. 取 wait_q 队首（最高优先级等待者）。
 * 3. 若其优先级高于 owner，调用 mutex_apply_inheritance 提升 owner。
 */
static void mutex_boost_from_waiters(cgrtos_mutex_t *mutex)
{
    if (!mutex->inherit || !mutex->owner || !mutex->wait_q) {
        return;
    }
    cgrtos_task_t *top = mutex->wait_q;
    if (top->prio > mutex->owner->prio) {
        mutex_apply_inheritance(mutex, top);
    }
}

/**
 * @brief 释放互斥量后将持有者优先级恢复为 base_prio
 * 
 * @param mutex 互斥量指针
 * 
 * @details
 * 1. 若未启用 inherit 或无 owner，直接返回。
 * 2. 若 owner->prio 已等于 base_prio，无需恢复。
 * 3. 若 owner 在 READY 队列，先 remove 再恢复 prio 后 re-add。
 */
static void mutex_restore_inheritance(cgrtos_mutex_t *mutex)
{
    if (!mutex->inherit || !mutex->owner) {
        return;
    }

    if (mutex->owner->prio != mutex->owner->base_prio) {
        if (mutex->owner->state == TASK_READY) {
            cgrtos_sched_remove_ready(mutex->owner);
        }
        mutex->owner->prio = mutex->owner->base_prio;
        if (mutex->owner->state == TASK_READY) {
            cgrtos_sched_add_ready(mutex->owner);
        }
    }
}

/**
 * @brief 获取互斥量锁
 * 
 * @param mutex   互斥量指针
 * @param timeout 阻塞超时 tick 数；0 表示不等待
 * 
 * @return pdPASS 成功加锁；pdFAIL 参数错误、非 owner 解锁或超时
 * 
 * @details
 * 1. 校验 mutex 非空并进入临界区。
 * 2. 若当前任务已是 owner，递增 recursive 计数并返回（递归锁）。
 * 3. 若无 owner，将当前任务设为 owner 并返回。
 * 4. 若 timeout==0 且锁被占用，返回 pdFAIL。
 * 5. 应用优先级继承后将当前任务挂入 wait_q 并 yield。
 * 6. 唤醒后通过 wait_result 返回结果。
 */
int cgrtos_mutex_lock(cgrtos_mutex_t *mutex, tick_t timeout)
{
    /* 1. 参数校验 */
    if (!mutex) {
        return pdFAIL;
    }

    /* 2. 进入临界区 */
    cgrtos_enter_critical();
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *cur = g_current[cpu];
    if (!cur) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    /* 3. 递归加锁：同一 owner 递增 recursive（受 CONFIG_MUTEX_MAX_RECURSIVE 限制） */
    if (mutex->owner == cur) {
        if (mutex->recursive >= CONFIG_MUTEX_MAX_RECURSIVE) {
            cgrtos_exit_critical();
            CGRTOS_ASSERT(mutex->recursive < CONFIG_MUTEX_MAX_RECURSIVE);
            return pdFAIL;
        }
        mutex->recursive++;
        cgrtos_exit_critical();
        return pdPASS;
    }

    /* 4. 锁空闲则直接获取 */
    if (!mutex->owner) {
        mutex->owner = cur;
        mutex->owner_prio = cur->prio;
        cgrtos_exit_critical();
        return pdPASS;
    }

    /* 5. 非阻塞模式立即失败 */
    if (timeout == 0) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    /* 6. 应用 PI 并挂入等待队列 */
    mutex_apply_inheritance(mutex, cur);
    block_current_on_waitq(&mutex->wait_q, BLOCK_MUTEX, mutex, timeout);
    cgrtos_exit_critical();

    cgrtos_sched_yield();
    return wait_result(cur);
}

/**
 * @brief 释放互斥量锁
 * 
 * @param mutex 互斥量指针
 * 
 * @return pdPASS 成功；pdFAIL 参数错误或当前任务非 owner
 * 
 * @details
 * 1. 校验 mutex 非空并进入临界区，确认当前任务为 owner。
 * 2. 若 recursive>0，递减后返回（递归解锁）。
 * 3. 恢复持有者优先级（mutex_restore_inheritance），清除 owner。
 * 4. 若有等待者，将其设为 owner 并 wake_ok=1 unblock。
 * 5. 对新 owner 执行 mutex_boost_from_waiters 处理剩余等待者的 PI。
 * 6. 唤醒等待者时 yield 以立即调度。
 */
int cgrtos_mutex_unlock(cgrtos_mutex_t *mutex)
{
    /* 1. 参数校验 */
    if (!mutex) {
        return pdFAIL;
    }

    /* 2. 进入临界区，校验 owner */
    cgrtos_enter_critical();
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *cur = g_current[cpu];

    if (mutex->owner != cur) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    /* 3. 递归解锁 */
    if (mutex->recursive > 0) {
        mutex->recursive--;
        cgrtos_exit_critical();
        return pdPASS;
    }

    /* 4. 恢复 PI 并释放所有权 */
    mutex_restore_inheritance(mutex);
    mutex->owner = 0;

    /* 5. 唤醒最高优先级等待者 */
    cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&mutex->wait_q);
    if (waiter) {
        mutex->owner = waiter;
        mutex->owner_prio = waiter->base_prio;
        /* Remaining waiters may still require PI on the new owner. */
        mutex_boost_from_waiters(mutex);
        waiter->wake_ok = 1;
        cgrtos_sched_unblock(waiter);
        cgrtos_exit_critical();
        cgrtos_sched_yield();
        return pdPASS;
    }

    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 删除互斥量并唤醒所有等待者
 * 
 * @param mutex 互斥量指针
 * 
 * @return pdPASS 成功；pdFAIL 参数错误
 * 
 * @details
 * 1. 校验 mutex 非空并进入临界区。
 * 2. 逐个弹出 wait_q 等待者，置 wake_ok=0 并 unblock。
 * 3. 清零结构体并递减 g_mtx_cnt。
 * 4. 退出临界区后 yield。
 */
int cgrtos_mutex_delete(cgrtos_mutex_t *mutex)
{
    if (!mutex) {
        return pdFAIL;
    }

    cgrtos_enter_critical();
    while (mutex->wait_q) {
        cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&mutex->wait_q);
        if (waiter) {
            waiter->wake_ok = 0;
            cgrtos_sched_unblock(waiter);
        }
    }
    memset(mutex, 0, sizeof(*mutex));
    if (g_mtx_cnt > 0) {
        g_mtx_cnt--;
    }
    cgrtos_exit_critical();
    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 查询互斥量递归额外层数
 * @param mutex 互斥量指针
 * @return recursive 字段；mutex 为空返回 0
 * @details
 * 1. 临界区内读取 recursive。
 * 2. 语义：总持有次数 = recursive + 1（当 owner 非空）；无人持有时为 0。
 */
uint32_t cgrtos_mutex_get_recursive_count(cgrtos_mutex_t *mutex)
{
    if (!mutex) {
        return 0;
    }
    cgrtos_enter_critical();
    uint32_t r = mutex->owner ? mutex->recursive : 0;
    cgrtos_exit_critical();
    return r;
}

/**
 * @brief 查询互斥量当前持有者
 * @param mutex 互斥量指针
 * @return owner TCB；无人持有或参数非法返回 NULL
 * @details
 * 1. 临界区内快照 owner 指针并返回。
 */
cgrtos_task_t *cgrtos_mutex_get_holder(cgrtos_mutex_t *mutex)
{
    if (!mutex) {
        return 0;
    }
    cgrtos_enter_critical();
    cgrtos_task_t *o = mutex->owner;
    cgrtos_exit_critical();
    return o;
}

/**
 * @brief 强制释放指定任务持有的全部互斥量（任务删除安全）
 * @param task 即将删除 / 已标记删除的任务
 * @details
 * 1. task 为空则返回。
 * 2. 扫描 g_mtxs[]：对 owner==task 的互斥量：
 *    a. 调用 mutex_restore_inheritance 恢复 PI；
 *    b. 清零 owner / recursive；
 *    c. 若有等待者：handoff 给最高优先级等待者（wake_ok=1），并 boost；
 *    d. 否则锁空闲。
 * 3. 调用方须已在临界区，或本函数自行 enter/exit（本实现自带临界区）。
 * @note 防止“持锁任务被删导致等待者永久阻塞”。
 */
void cgrtos_mutex_force_release_owned(cgrtos_task_t *task)
{
    if (!task) {
        return;
    }

    /* 1. 进入临界区扫描池 */
    cgrtos_enter_critical();
    for (uint32_t i = 0; i < CGRTOS_MAX_MUTEX; i++) {
        cgrtos_mutex_t *m = &g_mtxs[i];
        if (!m->in_use || m->owner != task) {
            continue;
        }

        /* 2a. 恢复被提升的优先级 */
        mutex_restore_inheritance(m);
        m->owner = 0;
        m->recursive = 0;

        /* 2c. handoff 给最高优先级等待者 */
        cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&m->wait_q);
        if (waiter) {
            m->owner = waiter;
            m->owner_prio = waiter->base_prio;
            mutex_boost_from_waiters(m);
            waiter->wake_ok = 1;
            cgrtos_sched_unblock(waiter);
        }
    }
    cgrtos_exit_critical();
}

/**
 * @brief 动态创建定长消息队列
 * 
 * @param len     队列可容纳的消息条数
 * @param item_sz 每条消息的字节大小
 * 
 * @return 成功返回队列指针；参数非法、内存不足或池满返回 NULL
 * 
 * @details
 * 1. 校验 len>0、item_sz>0 且 len*item_sz 不溢出。
 * 2. 进入临界区，在 g_qs[] 中查找 buf==NULL 的空槽。
 * 3. 从 TLSF 堆分配 len*item_sz 字节环形缓冲。
 * 4. 初始化队列字段并递增 g_q_cnt。
 */
cgrtos_queue_t *cgrtos_queue_create(uint32_t len, uint32_t item_sz)
{
    /* 1. 参数校验 */
    if (!len || !item_sz) {
        return 0;
    }
    if (item_sz > (~(uint32_t)0U) / len) {
        return 0;
    }

    /* 2. 在静态池中分配并 malloc 缓冲 */
    cgrtos_enter_critical();
    for (uint32_t i = 0; i < CGRTOS_MAX_QUEUE; i++) {
        if (!g_qs[i].buf) {
            void *buf = cgrtos_malloc(len * item_sz);
            if (!buf) {
                cgrtos_exit_critical();
                return 0;
            }
            memset(&g_qs[i], 0, sizeof(g_qs[i]));
            g_qs[i].buf = buf;
            g_qs[i].len = len;
            g_qs[i].item_sz = item_sz;
            g_qs[i].storage_static = 0;
            g_q_cnt++;
            cgrtos_exit_critical();
            return &g_qs[i];
        }
    }
    cgrtos_exit_critical();
    return 0;
}

/**
 * @brief 在调用者提供的静态存储上初始化消息队列
 * 
 * @param q       调用者分配的队列结构体指针
 * @param storage 调用者提供的环形缓冲存储区
 * @param len     队列容量（条数）
 * @param item_sz 每条消息字节大小
 * 
 * @return 成功返回 q；参数非法返回 NULL
 * 
 * @details
 * 1. 校验 q、storage、len、item_sz 均非零。
 * 2. 清零结构体，绑定 buf、len、item_sz，标记 storage_static=1。
 * 3. 返回 q（缓冲由调用者管理，delete 时不 free）。
 */
cgrtos_queue_t *cgrtos_queue_create_static(cgrtos_queue_t *q, void *storage,
uint32_t len, uint32_t item_sz)
{
    if (!q || !storage || !len || !item_sz) {
        return 0;
    }
    memset(q, 0, sizeof(*q));
    q->buf = storage;
    q->len = len;
    q->item_sz = item_sz;
    q->storage_static = 1;
    return q;
}

/**
 * @brief 向环形队列尾部写入一条消息
 * 
 * @param q    队列指针
 * @param data 待写入消息数据（长度 item_sz 字节）
 * 
 * @return pdPASS
 * 
 * @details
 * 1. 将 data 拷贝到 buf[head * item_sz]。
 * 2. head 取模递增，cnt 加一。
 */
static int queue_push(cgrtos_queue_t *q, const void *data)
{
    memcpy((uint8_t *)q->buf + q->head * q->item_sz, data, q->item_sz);
    q->head = (q->head + 1) % q->len;
    q->cnt++;
    return pdPASS;
}

/**
 * @brief 从环形队列头部读出一条消息
 * 
 * @param q   队列指针
 * @param buf 接收缓冲区（至少 item_sz 字节）
 * 
 * @return pdPASS
 * 
 * @details
 * 1. 从 buf[tail * item_sz] 拷贝到 buf。
 * 2. tail 取模递增，cnt 减一。
 */
static int queue_pop(cgrtos_queue_t *q, void *buf)
{
    memcpy(buf, (uint8_t *)q->buf + q->tail * q->item_sz, q->item_sz);
    q->tail = (q->tail + 1) % q->len;
    q->cnt--;
    return pdPASS;
}

/**
 * @brief 队列发送内部实现（假定队列未满，在临界区内调用）
 * 
 * @param q          队列指针
 * @param data       待发送消息
 * @param need_yield 若非 NULL 且唤醒接收者则置 1
 * 
 * @return pdPASS 成功；errQUEUE_FULL 队列已满
 * 
 * @details
 * 1. 若 cnt<len，调用 queue_push 写入消息。
 * 2. 若有 recv_wait_q 等待者，弹出最高优先级者并 unblock。
 * 3. 若挂接 QueueSet 则 poke 通知。
 * 4. 队列已满返回 errQUEUE_FULL。
 */
static int queue_send_internal(cgrtos_queue_t *q, const void *data, int *need_yield)
{
    if (q->cnt < q->len) {
        queue_push(q, data);
        cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&q->recv_wait_q);
        if (waiter) {
            waiter->wake_ok = 1;
            cgrtos_sched_unblock(waiter);
            if (need_yield) {
                *need_yield = 1;
            }
        }
        if (q->qset) {
            cgrtos_queue_set_poke(q->qset, q);
        }
        return pdPASS;
    }
    return errQUEUE_FULL;
}

/**
 * @brief 向队列发送一条消息
 * 
 * @param q       队列指针
 * @param data    消息数据指针
 * @param timeout 队列满时的阻塞超时；0 表示不等待
 * 
 * @return pdPASS 成功；errQUEUE_FULL 非阻塞时满；pdFAIL 参数错误或超时
 * 
 * @details
 * 1. 校验 q、data 非空。
 * 2. 循环：进入临界区，若未满则 queue_send_internal 发送并返回。
 * 3. 若 timeout==0 且队列满，返回 errQUEUE_FULL。
 * 4. 将当前任务挂入 send_wait_q 并 yield。
 * 5. 唤醒后将 timeout 置 0 重试（防止被其他发送者抢占槽位）。
 */
int cgrtos_queue_send(cgrtos_queue_t *q, const void *data, tick_t timeout)
{
    /* 1. 参数校验 */
    if (!q || !data) {
        return pdFAIL;
    }

    for (;;) {
        int need_yield = 0;

        /* 2. 进入临界区 */
        cgrtos_enter_critical();
        uint8_t cpu = (uint8_t)read_csr(mhartid);
        cgrtos_task_t *cur = g_current[cpu];

        /* 3. 有空间则直接发送 */
        if (q->cnt < q->len) {
            int rc = queue_send_internal(q, data, &need_yield);
            cgrtos_exit_critical();
            if (need_yield) {
                cgrtos_sched_yield();
            }
            return rc;
        }

        /* 4. 非阻塞模式立即失败 */
        if (timeout == 0) {
            cgrtos_exit_critical();
            return errQUEUE_FULL;
        }

        /* 5. 挂入发送等待队列并 yield */
        block_current_on_waitq(&q->send_wait_q, BLOCK_QUEUE_SEND, q, timeout);
        cgrtos_exit_critical();
        cgrtos_sched_yield();

        if (wait_result(cur) != pdPASS) {
            return pdFAIL;
        }
        /* Woken: retry until success or another waiter raced the slot away. */
        timeout = 0;
    }
}

/**
 * @brief 从中断上下文向队列发送一条消息
 * 
 * @param q    队列指针
 * @param data 消息数据指针
 * 
 * @return pdPASS 成功；errQUEUE_FULL 队列满；pdFAIL 参数错误
 * 
 * @details
 * 1. 校验 q、data 非空并进入临界区。
 * 2. 若未满则 queue_send_internal 发送。
 * 3. 若满返回 errQUEUE_FULL。
 * 4. 退出临界区后若唤醒接收者则 sched_yield_from_isr。
 */
int cgrtos_queue_send_from_isr(cgrtos_queue_t *q, const void *data)
{
    if (!q || !data) {
        return pdFAIL;
    }

    int need_yield = 0;
    cgrtos_enter_critical();
    int rc = pdFAIL;
    if (q->cnt < q->len) {
        rc = queue_send_internal(q, data, &need_yield);
    } else {
        rc = errQUEUE_FULL;
    }
    cgrtos_exit_critical();
    if (need_yield) {
        cgrtos_sched_yield_from_isr();
    }
    return rc;
}

/**
 * @brief 从队列接收一条消息
 * 
 * @param q       队列指针
 * @param buf     接收缓冲区
 * @param timeout 队列空时的阻塞超时；0 表示不等待
 * 
 * @return pdPASS 成功；errQUEUE_EMPTY 非阻塞时空；pdFAIL 参数错误或超时
 * 
 * @details
 * 1. 校验 q、buf 非空。
 * 2. 循环：进入临界区，若 cnt>0 则 queue_pop 并唤醒 send_wait_q 等待者。
 * 3. 若 timeout==0 且队列空，返回 errQUEUE_EMPTY。
 * 4. 挂入 recv_wait_q 并 yield，唤醒后 timeout 置 0 重试。
 */
int cgrtos_queue_receive(cgrtos_queue_t *q, void *buf, tick_t timeout)
{
    if (!q || !buf) {
        return pdFAIL;
    }

    for (;;) {
        cgrtos_enter_critical();
        uint8_t cpu = (uint8_t)read_csr(mhartid);
        cgrtos_task_t *cur = g_current[cpu];

        if (q->cnt > 0) {
            queue_pop(q, buf);
            cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&q->send_wait_q);
            if (waiter) {
                waiter->wake_ok = 1;
                cgrtos_sched_unblock(waiter);
            }
            cgrtos_exit_critical();
            if (waiter) {
                cgrtos_sched_yield();
            }
            return pdPASS;
        }

        if (timeout == 0) {
            cgrtos_exit_critical();
            return errQUEUE_EMPTY;
        }

        block_current_on_waitq(&q->recv_wait_q, BLOCK_QUEUE_RECV, q, timeout);
        cgrtos_exit_critical();
        cgrtos_sched_yield();

        if (wait_result(cur) != pdPASS) {
            return pdFAIL;
        }
        timeout = 0;
    }
}

/**
 * @brief 从中断上下文从队列接收一条消息
 * 
 * @param q   队列指针
 * @param buf 接收缓冲区
 * 
 * @return pdPASS 成功；errQUEUE_EMPTY 队列空
 * 
 * @details
 * 1. 校验 q、buf 非空并进入临界区。
 * 2. 若 cnt==0 返回 errQUEUE_EMPTY。
 * 3. queue_pop 取出消息，若有 send_wait_q 等待者则唤醒。
 * 4. 退出临界区后若唤醒发送者则 sched_yield_from_isr。
 */
int cgrtos_queue_receive_from_isr(cgrtos_queue_t *q, void *buf)
{
    if (!q || !buf) {
        return errQUEUE_EMPTY;
    }

    cgrtos_enter_critical();
    if (q->cnt == 0) {
        cgrtos_exit_critical();
        return errQUEUE_EMPTY;
    }
    queue_pop(q, buf);
    cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&q->send_wait_q);
    if (waiter) {
        waiter->wake_ok = 1;
        cgrtos_sched_unblock(waiter);
    }
    cgrtos_exit_critical();

    if (waiter) {
        cgrtos_sched_yield_from_isr();
    }
    return pdPASS;
}

/**
 * @brief 查询队列中待接收的消息条数
 * 
 * @param q 队列指针
 * 
 * @return 消息条数；q 为 NULL 时返回 0
 * 
 * @details
 * 1. 校验 q 非空。
 * 2. 临界区内读取 cnt 并返回。
 */
uint32_t cgrtos_queue_messages_waiting(cgrtos_queue_t *q)
{
    if (!q) {
        return 0;
    }
    cgrtos_enter_critical();
    uint32_t cnt = q->cnt;
    cgrtos_exit_critical();
    return cnt;
}

/**
 * @brief 删除队列并释放资源
 * 
 * @param q 队列指针
 * 
 * @return pdPASS 成功；pdFAIL 参数错误
 * 
 * @details
 * 1. 校验 q 非空并进入临界区。
 * 2. 唤醒 recv_wait_q 与 send_wait_q 中所有等待者（wake_ok=0）。
 * 3. 保存 buf 指针与 storage_static 标志，清零结构体并递减 g_q_cnt。
 * 4. 退出临界区后，若非静态存储则 free(buf)。
 * 5. 若有被唤醒任务则 yield。
 */
int cgrtos_queue_delete(cgrtos_queue_t *q)
{
    if (!q) {
        return pdFAIL;
    }

    int need_yield = 0;
    cgrtos_enter_critical();

    cgrtos_task_t *w;
    while ((w = cgrtos_wait_list_pop_highest(&q->recv_wait_q)) != 0) {
        w->wake_ok = 0;
        cgrtos_sched_unblock(w);
        need_yield = 1;
    }
    while ((w = cgrtos_wait_list_pop_highest(&q->send_wait_q)) != 0) {
        w->wake_ok = 0;
        cgrtos_sched_unblock(w);
        need_yield = 1;
    }

    void *buf = q->buf;
    int was_static = q->storage_static;
    memset(q, 0, sizeof(*q));
    if (g_q_cnt > 0) {
        g_q_cnt--;
    }
    cgrtos_exit_critical();

    if (buf && !was_static) {
        cgrtos_free(buf);
    }
    if (need_yield) {
        cgrtos_sched_yield();
    }
    return pdPASS;
}

/**
 * @brief 动态创建事件组
 * 
 * @return 成功返回事件组指针；池满返回 NULL
 * 
 * @details
 * 1. 进入临界区，在 g_egs[] 中查找 in_use==0 的空槽。
 * 2. 清零结构体，设置 in_use=1，递增 g_eg_cnt。
 * 3. 返回新实例指针。
 */
cgrtos_event_group_t *cgrtos_event_group_create(void)
{
    cgrtos_enter_critical();
    for (uint32_t i = 0; i < CGRTOS_MAX_EVENT; i++) {
        if (!g_egs[i].in_use) {
            memset(&g_egs[i], 0, sizeof(g_egs[i]));
            g_egs[i].in_use = 1;
            g_eg_cnt++;
            cgrtos_exit_critical();
            return &g_egs[i];
        }
    }
    cgrtos_exit_critical();
    return 0;
}

/**
 * @brief 在调用者提供的静态存储上初始化事件组
 * 
 * @param eg 调用者分配的事件组结构体指针
 * 
 * @return 成功返回 eg；参数非法返回 NULL
 * 
 * @details
 * 1. 校验 eg 非空。
 * 2. 清零结构体，设置 in_use=1。
 * 3. 返回 eg（不占用全局池）。
 */
cgrtos_event_group_t *cgrtos_event_group_create_static(cgrtos_event_group_t *eg)
{
    if (!eg) {
        return 0;
    }
    memset(eg, 0, sizeof(*eg));
    eg->in_use = 1;
    return eg;
}

/**
 * @brief 判断事件组当前标志是否满足任务的等待条件
 * 
 * @param eg   事件组指针
 * @param task 等待中的任务
 * 
 * @return 1 条件满足；0 不满足或 task 非 EVENT 阻塞
 * 
 * @details
 * 1. 校验 task 有效且 block_reason 为 BLOCK_EVENT。
 * 2. 若 event_wait_all 为真，检查 (flags & mask) == mask（AND 模式）。
 * 3. 否则检查 (flags & mask) != 0（OR 模式）。
 */
static int event_match(cgrtos_event_group_t *eg, cgrtos_task_t *task)
{
    if (!task || task->block_reason != BLOCK_EVENT) {
        return 0;
    }
    if (task->event_wait_all) {
        return (eg->flags & task->event_wait_mask) == task->event_wait_mask;
    }
    return (eg->flags & task->event_wait_mask) != 0;
}

/**
 * @brief 设置事件组标志位
 * 
 * @param eg    事件组指针
 * @param flags 要置位的标志掩码
 * 
 * @return 设置后的完整 flags 值；eg 为 NULL 时返回 0
 * 
 * @details
 * 1. 校验 eg 非空并进入临界区。
 * 2. 执行 flags |= 待置位掩码。
 * 3. 遍历 wait_q，对满足 event_match 的等待者 remove、wake_ok=1、unblock。
 * 4. 保存当前 flags，退出临界区后 yield。
 */
event_flags_t cgrtos_event_group_set(cgrtos_event_group_t *eg, event_flags_t flags)
{
    if (!eg) {
        return 0;
    }

    cgrtos_enter_critical();
    eg->flags |= flags;

    cgrtos_task_t *waiter = eg->wait_q;
    while (waiter) {
        cgrtos_task_t *next = waiter->next;
        if (event_match(eg, waiter)) {
            cgrtos_wait_list_remove(&eg->wait_q, waiter);
            waiter->wake_ok = 1;
            cgrtos_sched_unblock(waiter);
        }
        waiter = next;
    }

    event_flags_t out = eg->flags;
    cgrtos_exit_critical();
    cgrtos_sched_yield();
    return out;
}

/**
 * @brief 从中断上下文设置事件组标志位
 * 
 * @param eg    事件组指针
 * @param flags 要置位的标志掩码
 * 
 * @return 设置后的完整 flags 值；eg 为 NULL 时返回 0
 * 
 * @details
 * 1. 校验 eg 非空并进入临界区。
 * 2. 执行 flags |= 待置位掩码。
 * 3. 遍历 wait_q 唤醒满足条件的等待者。
 * 4. 退出临界区后 sched_yield_from_isr。
 */
event_flags_t cgrtos_event_group_set_from_isr(cgrtos_event_group_t *eg,
event_flags_t flags)
{
    if (!eg) {
        return 0;
    }

    cgrtos_enter_critical();
    eg->flags |= flags;

    cgrtos_task_t *waiter = eg->wait_q;
    while (waiter) {
        cgrtos_task_t *next = waiter->next;
        if (event_match(eg, waiter)) {
            cgrtos_wait_list_remove(&eg->wait_q, waiter);
            waiter->wake_ok = 1;
            cgrtos_sched_unblock(waiter);
        }
        waiter = next;
    }

    event_flags_t out = eg->flags;
    cgrtos_exit_critical();
    cgrtos_sched_yield_from_isr();
    return out;
}

/**
 * @brief 清除事件组指定标志位
 * 
 * @param eg    事件组指针
 * @param flags 要清除的标志掩码
 * 
 * @return 清除后的完整 flags 值；eg 为 NULL 时返回 0
 * 
 * @details
 * 1. 校验 eg 非空并进入临界区。
 * 2. 执行 flags &= ~待清除掩码。
 * 3. 返回当前 flags 并退出临界区。
 */
event_flags_t cgrtos_event_group_clear(cgrtos_event_group_t *eg, event_flags_t flags)
{
    if (!eg) {
        return 0;
    }
    cgrtos_enter_critical();
    eg->flags &= ~flags;
    event_flags_t out = eg->flags;
    cgrtos_exit_critical();
    return out;
}

/**
 * @brief 等待事件组标志（不清除）
 * 
 * @param eg       事件组指针
 * @param flags    等待的标志掩码
 * @param wait_all 1=AND 等待全部置位；0=OR 等待任一置位
 * @param timeout  阻塞超时 tick 数；0 表示不等待
 * 
 * @return 满足条件的 flags 子集；超时或未满足返回 0
 * 
 * @details
 * 1. 委托 cgrtos_event_group_wait_bits，clear_on_exit 固定为 0。
 */
event_flags_t cgrtos_event_group_wait(cgrtos_event_group_t *eg, event_flags_t flags,
uint8_t wait_all, tick_t timeout)
{
    return cgrtos_event_group_wait_bits(eg, flags, 0, wait_all, timeout);
}

/**
 * @brief 等待事件组标志（可选退出时清除）
 * 
 * @param eg            事件组指针
 * @param flags         等待的标志掩码
 * @param clear_on_exit 1=成功返回后清除已满足的 flags 位
 * @param wait_all      1=AND 等待；0=OR 等待
 * @param timeout       阻塞超时 tick 数；0 表示不等待
 * 
 * @return 满足条件的 flags 子集；超时或未满足返回 0
 * 
 * @details
 * 1. 校验 eg 非空并进入临界区。
 * 2. 按 wait_all 模式检查当前 flags 是否已满足。
 * 3. 已满足则返回匹配子集，可选清除对应位。
 * 4. timeout==0 且未满足则返回 0。
 * 5. 记录 event_wait_mask、event_wait_all 到当前任务并挂入 wait_q。
 * 6. yield 后再次检查 flags，返回结果并可选清除。
 */
event_flags_t cgrtos_event_group_wait_bits(cgrtos_event_group_t *eg, event_flags_t flags,
uint8_t clear_on_exit, uint8_t wait_all,
tick_t timeout)
{
    if (!eg) {
        return 0;
    }

    cgrtos_enter_critical();
    int matched = wait_all ? ((eg->flags & flags) == flags) :
    ((eg->flags & flags) != 0);
    if (matched) {
        event_flags_t out = eg->flags & flags;
        if (clear_on_exit) {
            eg->flags &= ~flags;
        }
        cgrtos_exit_critical();
        return out;
    }

    if (timeout == 0) {
        cgrtos_exit_critical();
        return 0;
    }

    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *cur = g_current[cpu];
    cur->block_obj = eg;
    cur->event_wait_mask = flags;
    cur->event_wait_all = wait_all;
    block_current_on_waitq(&eg->wait_q, BLOCK_EVENT, eg, timeout);
    cgrtos_exit_critical();

    cgrtos_sched_yield();

    cgrtos_enter_critical();
    matched = wait_all ? ((eg->flags & flags) == flags) :
    ((eg->flags & flags) != 0);
    event_flags_t out = matched ? (eg->flags & flags) : 0;
    if (matched && clear_on_exit) {
        eg->flags &= ~flags;
    }
    cgrtos_exit_critical();
    return out;
}

/**
 * @brief 读取事件组当前全部标志位
 * 
 * @param eg 事件组指针
 * 
 * @return 当前 flags；eg 为 NULL 时返回 0
 * 
 * @details
 * 1. 校验 eg 非空。
 * 2. 临界区内读取 flags 并返回。
 */
event_flags_t cgrtos_event_group_get(cgrtos_event_group_t *eg)
{
    if (!eg) {
        return 0;
    }
    cgrtos_enter_critical();
    event_flags_t out = eg->flags;
    cgrtos_exit_critical();
    return out;
}

/**
 * @brief 删除事件组并唤醒所有等待者
 * 
 * @param eg 事件组指针
 * 
 * @return pdPASS 成功；pdFAIL 参数错误
 * 
 * @details
 * 1. 校验 eg 非空并进入临界区。
 * 2. 逐个弹出 wait_q 等待者，置 wake_ok=0 并 unblock。
 * 3. 清零结构体并递减 g_eg_cnt。
 * 4. 退出临界区后若有被唤醒任务则 yield。
 */
int cgrtos_event_group_delete(cgrtos_event_group_t *eg)
{
    if (!eg) {
        return pdFAIL;
    }

    int need_yield = 0;
    cgrtos_enter_critical();
    cgrtos_task_t *w;
    while ((w = cgrtos_wait_list_pop_highest(&eg->wait_q)) != 0) {
        w->wake_ok = 0;
        cgrtos_sched_unblock(w);
        need_yield = 1;
    }
    memset(eg, 0, sizeof(*eg));
    if (g_eg_cnt > 0) {
        g_eg_cnt--;
    }
    cgrtos_exit_critical();
    if (need_yield) {
        cgrtos_sched_yield();
    }
    return pdPASS;
}
