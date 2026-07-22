/**
 * @file ipc.c
 * @brief 信号量、互斥量、消息队列与事件组 IPC 原语实现
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 本模块实现 CGRtOS 四类经典同步/通信对象，均基于优先级等待队列与调度器阻塞机制：
 *
 * - **信号量（Semaphore）**：计数型同步对象，`count` 表示可用令牌数；`take` 减计数或阻塞，
 *   `give` 优先唤醒最高优先级等待者，否则递增计数（不超过 `max`）。
 * - **互斥量（Mutex）**：带所有权与递归计数的互斥锁，默认启用优先级继承（PI）；
 *   高优先级任务阻塞时临时提升持有者优先级，释放后恢复 `base_prio`。
 * - **消息队列（Queue）**：定长环形缓冲，每项固定 `item_sz` 字节；独立维护
 *   `send_wait_q` / `recv_wait_q`，满/空时分别阻塞发送者与接收者。
 * - **事件组（Event Group）**：32 位标志位集合，支持 AND/OR 等待模式及退出时清除选项。
 *
 * 全局静态池 `g_sems[]`、`g_mtxs[]`、`g_qs[]`、`g_egs[]` 管理实例生命周期；
 * 各对象可挂接 QueueSet（`qset` 字段）。流缓冲/消息缓冲/队列集/文件系统/定时器见独立模块。
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
 * @details 读取 running 任务；idle 不可阻塞。调用 sched_block 并 wait_list_add；须在 exit_critical 后 yield。
 * @param[in,out] wait_q 目标等待队列指针（各 IPC 对象的 wait_q 成员）
 * @param[in] reason 阻塞原因（BLOCK_SEM / BLOCK_MUTEX / BLOCK_QUEUE_* / BLOCK_EVENT）
 * @param[in] obj 关联 IPC 对象指针，写入 task->block_obj
 * @param[in] timeout 阻塞超时 tick 数
 * @return pdPASS 成功挂起；pdFAIL 当前无有效任务
 * @retval pdPASS 已挂入等待队列
 * @retval pdFAIL 当前为 idle 或无 running 任务
 * @note 调用方须在 exit_critical 后 cgrtos_sched_yield
 * @warning 须在临界区 / g_klock 内调用
 * @attention ❌ 单独调用非 ISR 安全；✅ 后续由调用方 yield 切换
 * @internal
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
 * @details yield 返回后读取 task->wake_ok 判定阻塞是否满足条件。
 * @param[in] task 被阻塞后重新运行的任务指针
 * @return pdPASS 唤醒成功；errPARAM 或 errTIMEOUT 失败
 * @retval pdPASS 条件满足（wake_ok==1）
 * @retval errPARAM task 为空
 * @retval errTIMEOUT 超时或被 delete 唤醒
 * @note 须在 unblock 后、任务再次运行时使用
 * @warning 无
 * @attention ✅ 任务上下文；❌ 不阻塞
 * @internal
 */
static int wait_result(cgrtos_task_t *task)
{
    if (!task) {
        return errPARAM;
    }
    return task->wake_ok ? pdPASS : errTIMEOUT;
}

#if CONFIG_DETECT_DEADLOCK
/**
 * @brief 检测 mutex 等待环：owner 链上若回到 self 则死锁
 * @details 沿 mutex->owner 及 owner 阻塞的 mutex 链遍历，深度上限 16。
 * @param[in] mutex 待检测互斥量
 * @param[in] self  当前尝试加锁的任务
 * @return 1 检测到环；0 无环或链终止
 * @retval 1 存在死锁环
 * @retval 0 无环、参数无效或链终止
 * @note 调用方已持 g_klock
 * @warning 深度截断可能漏检超长链
 * @attention ❌ 仅临界区内；❌ 不阻塞
 * @internal
 */
static int mutex_would_deadlock(cgrtos_mutex_t *mutex, cgrtos_task_t *self)
{
    cgrtos_task_t *t;
    int depth = 0;
    if (!mutex || !self) {
        return 0;
    }
    t = mutex->owner;
    while (t && depth < 16) {
        if (t == self) {
            return 1;
        }
        if (t->state != TASK_BLOCKED || t->block_reason != BLOCK_MUTEX ||
            !t->block_obj) {
            break;
        }
        t = ((cgrtos_mutex_t *)t->block_obj)->owner;
        depth++;
    }
    return 0;
}
#endif

/**
 * @brief 创建计数信号量（池分配）
 * @details 在 g_sems[] 找空槽，初始化 count/max/wait_q；校验 max>0 且 0<=init<=max。
 * @param[in] init 初值；须 0..max
 * @param[in] max  上限；须 >0
 * @return 信号量指针
 * @retval 非 NULL 成功
 * @retval NULL    参数非法或池满
 * @note 对象在静态池中，用户不得 free 指针
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
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
 * @brief 创建二进制信号量
 * @details 等价 cgrtos_sem_create(0, 1)，初始为空、最大为 1。
 * @return 信号量指针
 * @retval 非 NULL 成功
 * @retval NULL    池满
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_sem_t *cgrtos_sem_create_binary(void)
{
    return cgrtos_sem_create(0, 1);
}

/**
 * @brief 在调用者存储上静态创建信号量
 * @details 不占用全局池；清零结构体并设置 count/max，调用方保证 sem 生命周期。
 * @param[out] sem  用户提供的对象存储；不可为 NULL
 * @param[in]  init 初值
 * @param[in]  max  上限
 * @return sem 或 NULL
 * @retval sem  成功
 * @retval NULL 参数非法
 * @note 无
 * @warning 勿对池对象与静态对象混用 delete 语义错误
 * @attention ❌ ISR；❌ 不阻塞
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
 * @details count>0 则递减立即返回；否则按 timeout 挂入优先级等待队列并 yield，
 *          由 give 或超时唤醒。CONFIG_ISR_API_GUARD 开启时拒绝 ISR 调用。
 * @param[in] sem     信号量对象；不可为 NULL
 * @param[in] timeout 0=非阻塞尝试；portMAX_DELAY=永久等待；其它=相对 tick
 * @return 结果码
 * @retval pdPASS     成功取得令牌
 * @retval errPARAM   sem 为空
 * @retval errISR     在中断上下文调用
 * @retval errTIMEOUT 超时或非阻塞未取到
 * @note 等待队列按任务优先级排序；临界区保护 count/wait_q
 * @warning 无所有权概念，过度 give 受 max 限制；与 mutex 混用场景需自行设计协议
 * @attention ❌ 禁止 ISR（阻塞路径）；✅ timeout>0 且无令牌时阻塞并可能切换
 */
int cgrtos_sem_take(cgrtos_sem_t *sem, tick_t timeout)
{
    /* 1. 参数校验 */
    if (!sem) {
        return errPARAM;
    }
#if CONFIG_ISR_API_GUARD
    if (cgrtos_reject_blocking_in_isr()) {
        return errISR;
    }
#endif

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
        return errTIMEOUT;
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
 * @brief 释放信号量（V / give）
 * @details 有等待者则唤醒最高优先级并 yield；否则 count++（不超过 max）；可能 poke QueueSet。
 * @param[in] sem 信号量；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数错误
 * @note 无所有权概念，过度 give 受 max 限制
 * @warning 无
 * @attention ❌ ISR；✅ 唤醒等待者时可能切换
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
 * @brief ISR 中释放信号量
 * @details 不阻塞；可 unblock 等待者并通过 cgrtos_isr_notify_woken 置 woken / 自动 yield_from_isr。
 * @param[in]  sem   信号量；不可为 NULL
 * @param[out] woken 可选；唤醒更高优先级等待者时置 pdTRUE
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数错误
 * @note 无
 * @warning 忽略 woken 且未自动 yield 可能导致延迟调度
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_sem_give_from_isr(cgrtos_sem_t *sem, BaseType_t *woken)
{
    if (!sem) {
        return pdFAIL;
    }

    int need_yield = 0;
    cgrtos_enter_critical();

    cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&sem->wait_q);
    if (waiter) {
        waiter->wake_ok = 1;
        cgrtos_sched_unblock(waiter);
        need_yield = 1;
        if (sem->qset) {
            cgrtos_queue_set_poke(sem->qset, sem);
        }
        cgrtos_exit_critical();
        cgrtos_isr_notify_woken(woken, need_yield);
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
 * @brief ISR 中非阻塞获取信号量
 * @details 仅当 count>0 时递减；从不等待、不唤醒任何任务。
 * @param[in] sem 信号量；不可为 NULL
 * @return 结果码
 * @retval pdPASS 取到令牌
 * @retval pdFAIL 无令牌或参数错误
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_sem_take_from_isr(cgrtos_sem_t *sem)
{
    if (!sem) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    if (sem->count > 0) {
        sem->count--;
        cgrtos_exit_critical();
        return pdPASS;
    }
    cgrtos_exit_critical();
    return pdFAIL;
}

/**
 * @brief 删除信号量并唤醒等待者（失败唤醒）
 * @details 等待者 wake_ok=0 并 unblock；清零结构体并回收池槽；exit 后 yield。
 * @param[in] sem 信号量；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数错误
 * @note 无
 * @warning 删除后禁止再使用指针
 * @attention ❌ ISR；✅ 可能切换
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
 * @brief 创建互斥量（默认启用优先级继承）
 * @details 在 g_mtxs[] 池分配空槽；清零结构体，设置 in_use=1、inherit=1。
 * @return 互斥量指针
 * @retval 非 NULL 成功
 * @retval NULL    池满
 * @note 支持递归加锁（上限 CONFIG_MUTEX_MAX_RECURSIVE）
 * @warning 勿在 ISR 中 lock
 * @attention ❌ ISR；❌ 不阻塞
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
 * @details 校验 mutex 非空；清零结构体，设置 in_use=1、inherit=1；不占用全局池。
 * @param[in] mutex 调用者分配的互斥量结构体指针；不可为 NULL
 * @return 互斥量指针
 * @retval 非 NULL 成功
 * @retval NULL    mutex 为空
 * @note delete 时不回收池槽（调用者管理存储）
 * @warning 同一 storage 未 delete 前勿重复 init
 * @attention ❌ ISR；❌ 不阻塞
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

#if CONFIG_USE_DPCP
/**
 * @brief 创建启用 DPCP 天花板的互斥量
 * @details 基于 cgrtos_mutex_create；inherit=0、dpcp=1，写入 ceiling_prio / ceiling_rel。
 * @param[in] ceiling_prio FP 优先级天花板 0..CONFIG_MAX_PRIORITY
 * @param[in] ceiling_rel  EDF 相对 deadline 天花板；0=仅用优先级天花板
 * @return 互斥量指针
 * @retval 非 NULL 成功
 * @retval NULL    参数非法或池满
 * @note 与动态 PI 互斥
 * @warning 天花板过低无法抑制反转；过高影响其他任务
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_mutex_t *cgrtos_mutex_create_dpcp(uint8_t ceiling_prio, tick_t ceiling_rel)
{
    if (ceiling_prio > CONFIG_MAX_PRIORITY) {
        return 0;
    }
    cgrtos_mutex_t *m = cgrtos_mutex_create();
    if (!m) {
        return 0;
    }
    m->inherit = 0;
    m->dpcp = 1;
    m->ceiling_prio = ceiling_prio;
    m->ceiling_rel = ceiling_rel;
    m->ceiling_applied = 0;
    return m;
}

/**
 * @brief 更新 DPCP 天花板（须无 owner）
 * @details 临界区内写入 ceiling_prio/ceiling_rel；若已有 owner 则 pdFAIL。
 * @param[in] mutex        互斥量；不可为 NULL
 * @param[in] ceiling_prio FP 优先级天花板 0..CONFIG_MAX_PRIORITY
 * @param[in] ceiling_rel  EDF 相对 deadline 天花板；0=仅用优先级天花板
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或 mutex 已被持有
 * @note 自动设置 dpcp=1、inherit=0
 * @warning 持锁期间修改天花板无效
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_mutex_set_ceiling(cgrtos_mutex_t *mutex, uint8_t ceiling_prio,
                             tick_t ceiling_rel)
{
    if (!mutex || ceiling_prio > CONFIG_MAX_PRIORITY) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    if (mutex->owner) {
        cgrtos_exit_critical();
        return pdFAIL;
    }
    mutex->dpcp = 1;
    mutex->inherit = 0;
    mutex->ceiling_prio = ceiling_prio;
    mutex->ceiling_rel = ceiling_rel;
    mutex->ceiling_applied = 0;
    cgrtos_exit_critical();
    return pdPASS;
}
#endif /* CONFIG_USE_DPCP */


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

#if CONFIG_USE_DPCP
/**
 * @brief DPCP：对 owner 应用天花板（优先级与/或 EDF deadline）
 * @details READY 时先出队再改 prio/deadline 再入队；跨核 RUNNING 时发 IPI。须持 g_klock。
 * @param[in,out] mutex 已加锁的 DPCP 互斥量
 * @return 无
 * @retval 无
 * @note 设置 ceiling_applied=1 并保存 saved_prio/saved_deadline
 * @warning 必须在 g_klock 内；READY 队列须 remove/add 避免乱序
 * @attention ❌ ISR；❌ 不阻塞；✅ 可能 IPI 触发远端重调度
 * @internal
 */
static void mutex_apply_dpcp(cgrtos_mutex_t *mutex)
{
    cgrtos_task_t *owner;
    if (!mutex || !mutex->dpcp || !mutex->owner || mutex->ceiling_applied) {
        return;
    }
    owner = mutex->owner;
    mutex->saved_prio = owner->prio;
    mutex->saved_deadline = owner->deadline;

    if (owner->state == TASK_READY) {
        cgrtos_sched_remove_ready(owner);
    }

    if (mutex->ceiling_prio > owner->prio) {
        owner->prio = mutex->ceiling_prio;
    }
#if CONFIG_USE_EDF
    if (owner->policy == SCHED_EDF && mutex->ceiling_rel != 0) {
        tick_t ceil_abs = g_ticks + mutex->ceiling_rel;
        if (owner->deadline > ceil_abs) {
            owner->deadline = ceil_abs;
        }
    }
#endif

    if (owner->state == TASK_READY) {
        cgrtos_sched_add_ready(owner);
    } else if (owner->state == TASK_RUNNING &&
               owner->run_cpu != (uint8_t)read_csr(mhartid) &&
               owner->run_cpu < CONFIG_NUM_CORES) {
        cgrtos_smp_send_ipi(owner->run_cpu);
    }
    mutex->ceiling_applied = 1;
}

/**
 * @brief DPCP：解锁时恢复 owner 的 prio/deadline
 * @details 从 saved_prio/saved_deadline 还原；READY 时先 remove 再 restore 再 add。
 * @param[in,out] mutex 已启用 DPCP 且 ceiling_applied 的互斥量
 * @return 无
 * @retval 无
 * @note 清除 ceiling_applied 标志
 * @warning 须在 g_klock 内调用
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
 */
static void mutex_restore_dpcp(cgrtos_mutex_t *mutex)
{
    cgrtos_task_t *owner;
    if (!mutex || !mutex->dpcp || !mutex->owner || !mutex->ceiling_applied) {
        return;
    }
    owner = mutex->owner;
    if (owner->state == TASK_READY) {
        cgrtos_sched_remove_ready(owner);
    }
    owner->prio = mutex->saved_prio;
#if CONFIG_USE_EDF
    owner->deadline = mutex->saved_deadline;
#endif
    if (owner->state == TASK_READY) {
        cgrtos_sched_add_ready(owner);
    }
    mutex->ceiling_applied = 0;
}
#endif /* CONFIG_USE_DPCP */

/**
 * @brief 优先级继承：将 mutex owner 提升至 waiter 优先级
 * @details DPCP mutex 跳过；waiter->prio 须高于 owner。READY 时更新就绪队列；跨核 RUNNING 发 IPI。
 * @param[in,out] mutex  互斥量
 * @param[in]     waiter 更高优先级等待者
 * @return 无
 * @retval 无
 * @note 仅 inherit 启用且非 DPCP 时生效
 * @warning 须在 g_klock 内调用
 * @attention ❌ ISR；❌ 不阻塞；✅ 可能 IPI
 * @internal
 */
static void mutex_apply_inheritance(cgrtos_mutex_t *mutex, cgrtos_task_t *waiter)
{
    /* 1. 检查 PI 前置条件（DPCP 用天花板，不做动态 PI） */
    if (!mutex->inherit || !mutex->owner || !waiter) {
        return;
    }
#if CONFIG_USE_DPCP
    if (mutex->dpcp) {
        return;
    }
#endif
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
 * @details wait_q 队首为最高优先级等待者；若其 prio 高于 owner 则调用 mutex_apply_inheritance。
 * @param[in,out] mutex 互斥量指针
 * @return 无
 * @retval 无
 * @note DPCP mutex 直接返回
 * @warning 须在 g_klock 内调用
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
 */
static void mutex_boost_from_waiters(cgrtos_mutex_t *mutex)
{
    if (!mutex->inherit || !mutex->owner || !mutex->wait_q) {
        return;
    }
#if CONFIG_USE_DPCP
    if (mutex->dpcp) {
        return;
    }
#endif
    cgrtos_task_t *top = mutex->wait_q;
    if (top->prio > mutex->owner->prio) {
        mutex_apply_inheritance(mutex, top);
    }
}

/**
 * @brief 释放互斥量后将持有者优先级恢复为 base_prio
 * @details owner->prio 不等于 base_prio 时，READY 状态先 remove 再恢复再 add。
 * @param[in,out] mutex 互斥量指针
 * @return 无
 * @retval 无
 * @note 无 inherit 或无 owner 时直接返回
 * @warning 须在 g_klock 内调用
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
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
 * @brief 获取互斥量锁（可递归）
 * @details 空闲则占用并可选应用 DPCP；同 owner 递增 recursive；否则执行 PI（非 DPCP）
 *          并阻塞等待。CONFIG_DETECT_DEADLOCK 开启时沿 owner 链检测等待环。
 * @param[in] mutex   互斥量；不可为 NULL
 * @param[in] timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 结果码
 * @retval pdPASS      加锁成功
 * @retval errPARAM    参数非法
 * @retval errISR      中断上下文调用
 * @retval errTIMEOUT  超时或非阻塞失败
 * @retval errDEADLOCK 检测到死锁环
 * @retval errOVERFLOW 递归层数超过 CONFIG_MUTEX_MAX_RECURSIVE
 * @note 必须与 unlock 严格成对；临界区保护 owner/wait_q
 * @warning 锁顺序不当仍可能死锁；持锁期间长时间阻塞其它资源会扩大优先级反转窗口
 * @attention ❌ 禁止 ISR；✅ 可能阻塞并引起上下文切换；可能抬升持有者优先级
 */
int cgrtos_mutex_lock(cgrtos_mutex_t *mutex, tick_t timeout)
{
    /* 1. 参数校验 */
    if (!mutex) {
        return errPARAM;
    }
#if CONFIG_ISR_API_GUARD
    if (cgrtos_reject_blocking_in_isr()) {
        return errISR;
    }
#endif

    /* 2. 进入临界区 */
    cgrtos_enter_critical();
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *cur = g_current[cpu];
    if (!cur) {
        cgrtos_exit_critical();
        return errPARAM;
    }

    /* 3. 递归加锁：同一 owner 递增 recursive（受 CONFIG_MUTEX_MAX_RECURSIVE 限制） */
    if (mutex->owner == cur) {
        if (mutex->recursive >= CONFIG_MUTEX_MAX_RECURSIVE) {
            cgrtos_exit_critical();
            CGRTOS_ASSERT(mutex->recursive < CONFIG_MUTEX_MAX_RECURSIVE);
            return errOVERFLOW;
        }
        mutex->recursive++;
        cgrtos_exit_critical();
        return pdPASS;
    }

    /* 4. 锁空闲则直接获取 */
    if (!mutex->owner) {
        mutex->owner = cur;
        mutex->owner_prio = cur->prio;
#if CONFIG_USE_DPCP
        if (mutex->dpcp) {
            mutex_apply_dpcp(mutex);
        }
#endif
        cgrtos_exit_critical();
        return pdPASS;
    }

    /* 5. 非阻塞模式立即失败 */
    if (timeout == 0) {
        cgrtos_exit_critical();
        return errTIMEOUT;
    }

#if CONFIG_DETECT_DEADLOCK
    /* 5b. 等待环检测 */
    if (mutex_would_deadlock(mutex, cur)) {
        cgrtos_exit_critical();
        CGRTOS_LOGE("mtx", "deadlock detected");
        return errDEADLOCK;
    }
#endif

    /* 6. 应用 PI 并挂入等待队列 */
    mutex_apply_inheritance(mutex, cur);
    block_current_on_waitq(&mutex->wait_q, BLOCK_MUTEX, mutex, timeout);
    cgrtos_exit_critical();

    cgrtos_sched_yield();
    return wait_result(cur);
}

/**
 * @brief 互斥量解锁
 * @details 递归则减层；否则恢复 PI/DPCP、清除 owner，并可能 handoff 给最高等待者后 yield。
 * @param[in] mutex 互斥量；不可为 NULL；当前任务须为 owner
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 非 owner 或参数非法
 * @note 须与 lock 成对调用
 * @warning 非持有者 unlock 失败
 * @attention ❌ ISR；✅ 可能切换
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

    /* 4. 恢复 DPCP/PI 并释放所有权 */
#if CONFIG_USE_DPCP
    if (mutex->dpcp) {
        mutex_restore_dpcp(mutex);
    } else
#endif
    {
        mutex_restore_inheritance(mutex);
    }
    mutex->owner = 0;

    /* 5. 唤醒最高优先级等待者 */
    cgrtos_task_t *waiter = cgrtos_wait_list_pop_highest(&mutex->wait_q);
    if (waiter) {
        mutex->owner = waiter;
        mutex->owner_prio = waiter->base_prio;
#if CONFIG_USE_DPCP
        if (mutex->dpcp) {
            mutex_apply_dpcp(mutex);
        } else
#endif
        {
            mutex_boost_from_waiters(mutex);
        }
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
 * @brief 删除互斥量
 * @details 唤醒 wait_q 中所有等待者（wake_ok=0）；清零结构体并回收池槽；exit 后 yield。
 * @param[in] mutex 互斥量；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数错误
 * @note 无
 * @warning 仍有 owner 时行为以实现为准，应先确保解锁
 * @attention ❌ ISR；✅ 可能切换
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
 * @details 临界区内读取 recursive；总持有次数 = recursive + 1（owner 非空时）；无人持有返回 0。
 * @param[in] mutex 互斥量指针
 * @return 递归层数（不含首次 lock）
 * @retval >=0 当前 recursive 值；mutex 为空返回 0
 * @note 只读快照
 * @warning 无
 * @attention ✅ ISR 可读；❌ 不阻塞
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
 * @details 临界区内快照 owner TCB 指针并返回。
 * @param[in] mutex 互斥量指针
 * @return 持有者任务指针
 * @retval 非 NULL 当前 owner
 * @retval NULL    无人持有或 mutex 为空
 * @note 只读快照
 * @warning 返回指针在 unlock 后可能失效
 * @attention ✅ ISR 可读；❌ 不阻塞
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
 * @details 扫描 g_mtxs[]：对 owner==task 的互斥量恢复 PI、清零 owner/recursive；若有等待者则 handoff 并 boost。
 * @param[in] task 即将删除的任务 TCB；NULL 则直接返回
 * @return 无
 * @retval 无
 * @note 由 cgrtos_task_delete 等路径调用；防止持锁任务被删导致永久阻塞
 * @warning 会改变等待者优先级继承状态
 * @attention ❌ ISR；✅ 可能 unblock 等待者（不主动 yield）
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
 * @brief 创建消息队列（池分配 + 堆缓冲）
 * @details 在 g_qs[] 找空槽，从 TLSF 堆分配 len*item_sz 环形缓冲并初始化字段。
 * @param[in] len     队列可容纳的消息条数；须 >0
 * @param[in] item_sz 每条消息的字节大小；须 >0
 * @return 队列指针
 * @retval 非 NULL 成功
 * @retval NULL    参数非法、内存不足或池满
 * @note 对象在静态池中，缓冲由 delete 时 free（非 static 队列）
 * @warning len*item_sz 溢出时拒绝创建
 * @attention ❌ ISR；❌ 不阻塞
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
 * @details 校验 q/storage/len/item_sz；清零结构体，绑定 buf，标记 storage_static=1；delete 时不 free 缓冲。
 * @param[in] q       调用者分配的队列结构体指针
 * @param[in] storage 环形缓冲存储区（至少 len * item_sz 字节）
 * @param[in] len     队列容量（条数）；须 >0
 * @param[in] item_sz 每条消息字节大小；须 >0
 * @return 队列指针
 * @retval 非 NULL 成功
 * @retval NULL    参数非法
 * @note 不占用全局池
 * @warning storage 生命周期须覆盖队列使用期
 * @attention ❌ ISR；❌ 不阻塞
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
 * @brief 向环形队列尾部写入一条消息（假定未满，临界区内）
 * @details 将 data 拷贝到 buf[head * item_sz]，head 取模递增，cnt 加一。
 * @param[in,out] q    队列指针
 * @param[in]     data 待写入消息（item_sz 字节）
 * @return pdPASS
 * @retval pdPASS 写入成功
 * @note 由 queue_send_internal 调用；调用方须保证 q->cnt < q->len
 * @warning 队列已满时行为未定义
 * @attention ❌ 仅临界区内；❌ 不阻塞
 * @internal
 */
static int queue_push(cgrtos_queue_t *q, const void *data)
{
    memcpy((uint8_t *)q->buf + q->head * q->item_sz, data, q->item_sz);
    q->head = (q->head + 1) % q->len;
    q->cnt++;
    return pdPASS;
}

/**
 * @brief 从环形队列头部读出一条消息（假定非空，临界区内）
 * @details 从 buf[tail * item_sz] 拷贝到 buf，tail 取模递增，cnt 减一。
 * @param[in,out] q   队列指针
 * @param[out]    buf 接收缓冲区（至少 item_sz 字节）
 * @return pdPASS
 * @retval pdPASS 读出成功
 * @note 由 queue receive 路径调用；调用方须保证 q->cnt > 0
 * @warning 队列为空时行为未定义
 * @attention ❌ 仅临界区内；❌ 不阻塞
 * @internal
 */
static int queue_pop(cgrtos_queue_t *q, void *buf)
{
    memcpy(buf, (uint8_t *)q->buf + q->tail * q->item_sz, q->item_sz);
    q->tail = (q->tail + 1) % q->len;
    q->cnt--;
    return pdPASS;
}

/**
 * @brief 队列发送内部实现（假定在临界区内调用）
 * @details 未满则 queue_push 并可能唤醒 recv 等待者；挂接 QueueSet 时 poke；满返回 errQUEUE_FULL。
 * @param[in,out] q          队列指针
 * @param[in]     data       待发送消息
 * @param[out]    need_yield 非 NULL 且唤醒接收者时置 1
 * @return pdPASS 成功；errQUEUE_FULL 队列已满
 * @retval pdPASS         写入并可能唤醒等待者
 * @retval errQUEUE_FULL  无空闲槽位
 * @note 由 cgrtos_queue_send / send_from_isr 调用
 * @warning 须在 enter_critical 内；满时不会阻塞
 * @attention ❌ 仅临界区内；✅ 可能标记 need_yield
 * @internal
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
 * @details 队列未满则写入并可能唤醒 recv 等待者；满时按 timeout 挂入 send_wait_q 并 yield 重试。
 * @param[in] q       队列；不可为 NULL
 * @param[in] data    消息数据指针（长度 item_sz 字节）；不可为 NULL
 * @param[in] timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 结果码
 * @retval pdPASS         成功
 * @retval errPARAM       q 或 data 为空
 * @retval errISR         ISR 中调用阻塞路径（CONFIG_ISR_API_GUARD）
 * @retval errQUEUE_FULL  非阻塞时队列满
 * @retval pdFAIL         超时或被 delete 唤醒
 * @note 唤醒后 timeout 置 0 重试，防止槽位被抢占
 * @warning data 指向的内存在拷贝完成前须保持有效
 * @attention ❌ 阻塞路径禁止 ISR；✅ timeout>0 且满时阻塞并切换
 */
int cgrtos_queue_send(cgrtos_queue_t *q, const void *data, tick_t timeout)
{
    /* 1. 参数校验 */
    if (!q || !data) {
        return errPARAM;
    }
#if CONFIG_ISR_API_GUARD
    if (timeout != 0 && cgrtos_reject_blocking_in_isr()) {
        return errISR;
    }
#endif

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
 * @details 临界区内若未满则 queue_send_internal；满则 errQUEUE_FULL；退出后 cgrtos_isr_notify_woken。
 * @param[in]  q     队列指针；不可为 NULL
 * @param[in]  data  消息数据（长度 item_sz）；不可为 NULL
 * @param[out] woken 可选；唤醒接收者时置 pdTRUE
 * @return 结果码
 * @retval pdPASS         成功
 * @retval errQUEUE_FULL  队列满
 * @retval pdFAIL         参数错误
 * @note 不阻塞
 * @warning data 须在拷贝完成前有效
 * @attention ✅ ISR；❌ 不阻塞；✅ 可能 yield_from_isr
 */
int cgrtos_queue_send_from_isr(cgrtos_queue_t *q, const void *data, BaseType_t *woken)
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
    cgrtos_isr_notify_woken(woken, need_yield);
    return rc;
}

/**
 * @brief 从队列接收一条消息
 * @details 队列非空则 queue_pop 并可能唤醒 send 等待者；空时按 timeout 挂入 recv_wait_q 并 yield 重试。
 * @param[in]  q       队列；不可为 NULL
 * @param[out] buf     接收缓冲区（至少 item_sz 字节）；不可为 NULL
 * @param[in]  timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 结果码
 * @retval pdPASS          成功
 * @retval errPARAM        q 或 buf 为空
 * @retval errISR          ISR 中调用阻塞路径（CONFIG_ISR_API_GUARD）
 * @retval errQUEUE_EMPTY  非阻塞时队列空
 * @retval pdFAIL          超时或被 delete 唤醒
 * @note 唤醒后 timeout 置 0 重试
 * @warning buf 须足够容纳 item_sz 字节
 * @attention ❌ 阻塞路径禁止 ISR；✅ timeout>0 且空时阻塞并切换
 */
int cgrtos_queue_receive(cgrtos_queue_t *q, void *buf, tick_t timeout)
{
    if (!q || !buf) {
        return errPARAM;
    }
#if CONFIG_ISR_API_GUARD
    if (timeout != 0 && cgrtos_reject_blocking_in_isr()) {
        return errISR;
    }
#endif

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
 * @details 临界区内 cnt==0 则 errQUEUE_EMPTY；否则 queue_pop 并可能唤醒 send 等待者。
 * @param[in]  q     队列指针
 * @param[out] buf   接收缓冲区（至少 item_sz 字节）
 * @param[out] woken 可选；唤醒发送者时置 pdTRUE
 * @return 结果码
 * @retval pdPASS          成功
 * @retval errQUEUE_EMPTY  队列空或参数非法
 * @note 不阻塞
 * @warning buf 须足够容纳 item_sz 字节
 * @attention ✅ ISR；❌ 不阻塞；✅ 可能 yield_from_isr
 */
int cgrtos_queue_receive_from_isr(cgrtos_queue_t *q, void *buf, BaseType_t *woken)
{
    if (!q || !buf) {
        return errQUEUE_EMPTY;
    }

    int need_yield = 0;
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
        need_yield = 1;
    }
    cgrtos_exit_critical();
    cgrtos_isr_notify_woken(woken, need_yield);
    return pdPASS;
}

/**
 * @brief 查询队列中待接收的消息条数
 * @details 临界区内读取 q->cnt 快照。
 * @param[in] q 队列指针
 * @return 待接收消息条数
 * @retval >=0 当前 cnt；q 为 NULL 时返回 0
 * @note 只读
 * @warning 并发 send/recv 时值为瞬时快照
 * @attention ✅ ISR 可读；❌ 不阻塞
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
 * @details 唤醒 recv/send 等待者（wake_ok=0）；非静态存储则 free 环形缓冲；清零结构体并回收池槽。
 * @param[in] q 队列；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数错误
 * @note storage_static=1 的队列不 free 缓冲
 * @warning 删除后禁止再使用指针
 * @attention ❌ ISR；✅ 可能切换
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
 * @details 在 g_egs[] 池分配空槽；清零结构体，设置 in_use=1。
 * @return 事件组指针
 * @retval 非 NULL 成功
 * @retval NULL    池满
 * @note 对象在静态池中，用户不得 free 指针
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
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
 * @details 校验 eg 非空；清零结构体，设置 in_use=1；不占用全局池。
 * @param[in] eg 调用者分配的事件组结构体指针
 * @return 事件组指针
 * @retval 非 NULL 成功
 * @retval NULL    eg 为空
 * @note delete 时不回收池槽
 * @warning 同一 storage 未 delete 前勿重复 init
 * @attention ❌ ISR；❌ 不阻塞
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
 * @details 校验 block_reason==BLOCK_EVENT；AND 模式要求 (flags&mask)==mask，OR 模式要求 (flags&mask)!=0。
 * @param[in] eg   事件组指针
 * @param[in] task 等待中的任务
 * @return 1 条件满足；0 不满足或 task 非 EVENT 阻塞
 * @retval 1 等待条件已满足
 * @retval 0 不满足或参数无效
 * @note 由 event_group_set / set_from_isr 遍历 wait_q 时使用
 * @warning 须在 g_klock 内读取 flags 与 task 字段
 * @attention ❌ 仅临界区内；❌ 不阻塞
 * @internal
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
 * @details flags |= 掩码；遍历 wait_q 对满足 event_match 的等待者 unblock；exit 后 yield。
 * @param[in] eg    事件组；不可为 NULL
 * @param[in] flags 要置位的标志掩码
 * @return 设置后的完整 flags 值
 * @retval 非 0  成功（含置位后的位图）
 * @retval 0     eg 为 NULL
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能切换
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
 * @details eg->flags |= flags；遍历 wait_q 对满足 event_match 的等待者 unblock；cgrtos_isr_notify_woken。
 * @param[in]  eg    事件组指针
 * @param[in]  flags 要置位的标志掩码
 * @param[out] woken 可选；唤醒等待者时置 pdTRUE
 * @return 设置后的完整 flags 值
 * @retval 非 0  成功
 * @retval 0     eg 为 NULL
 * @note 不阻塞
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞；✅ 可能 yield_from_isr
 */
event_flags_t cgrtos_event_group_set_from_isr(cgrtos_event_group_t *eg,
                                              event_flags_t flags,
                                              BaseType_t *woken)
{
    if (!eg) {
        return 0;
    }

    int need_yield = 0;
    cgrtos_enter_critical();
    eg->flags |= flags;

    cgrtos_task_t *waiter = eg->wait_q;
    while (waiter) {
        cgrtos_task_t *next = waiter->next;
        if (event_match(eg, waiter)) {
            cgrtos_wait_list_remove(&eg->wait_q, waiter);
            waiter->wake_ok = 1;
            cgrtos_sched_unblock(waiter);
            need_yield = 1;
        }
        waiter = next;
    }

    event_flags_t out = eg->flags;
    cgrtos_exit_critical();
    cgrtos_isr_notify_woken(woken, need_yield);
    return out;
}

/**
 * @brief ISR 清除事件组指定标志位（不唤醒等待者）
 * @details 临界区内 eg->flags &= ~flags；不遍历 wait_q、不 unblock。
 * @param[in] eg    事件组指针
 * @param[in] flags 要清除的标志掩码
 * @return 清除后的完整 flags 值
 * @retval 非 0  成功
 * @retval 0     eg 为 NULL
 * @note 清位不会使 AND/OR 等待条件变为真
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞；❌ 不唤醒
 */
event_flags_t cgrtos_event_group_clear_from_isr(cgrtos_event_group_t *eg,
                                                event_flags_t flags)
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
 * @brief 清除事件组指定标志位
 * @details 临界区内 eg->flags &= ~flags；不唤醒等待者。
 * @param[in] eg    事件组指针
 * @param[in] flags 要清除的标志掩码
 * @return 清除后的完整 flags 值
 * @retval 非 0  成功
 * @retval 0     eg 为 NULL
 * @note 任务上下文版本；与 clear_from_isr 语义一致但不走 woken 路径
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞；❌ 不唤醒
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
 * @details 委托 cgrtos_event_group_wait_bits，clear_on_exit 固定为 0。
 * @param[in] eg       事件组指针
 * @param[in] flags    等待的标志掩码
 * @param[in] wait_all 1=AND 全部置位；0=OR 任一置位
 * @param[in] timeout  0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 满足条件的 flags 子集
 * @retval 非 0  条件满足
 * @retval 0     超时或未满足
 * @note 成功返回时不自动清除位
 * @warning 多任务同时 wait 须自行设计协议
 * @attention ❌ 阻塞路径禁止 ISR；✅ timeout>0 且未满足时阻塞并切换
 */
event_flags_t cgrtos_event_group_wait(cgrtos_event_group_t *eg, event_flags_t flags,
uint8_t wait_all, tick_t timeout)
{
    return cgrtos_event_group_wait_bits(eg, flags, 0, wait_all, timeout);
}

/**
 * @brief 等待事件组标志（可选退出时清除）
 * @details 已满足则立即返回匹配子集；否则记录 wait_mask/wait_all 挂入 wait_q 并 yield 后再次检查。
 * @param[in] eg            事件组；不可为 NULL
 * @param[in] flags         等待的标志掩码
 * @param[in] clear_on_exit 1=成功返回后清除已满足的 flags 位
 * @param[in] wait_all      1=AND 等待全部置位；0=OR 等待任一置位
 * @param[in] timeout       0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 满足条件的 flags 子集
 * @retval 非 0  条件满足（flags 与当前位图的交集）
 * @retval 0     超时、未满足、参数非法或 ISR 阻塞路径
 * @note clear_on_exit 仅在成功返回时清除对应位
 * @warning 多个任务同时 wait 同一组时须自行设计同步协议
 * @attention ❌ 阻塞路径禁止 ISR；✅ timeout>0 且未满足时阻塞并切换
 */
event_flags_t cgrtos_event_group_wait_bits(cgrtos_event_group_t *eg, event_flags_t flags,
uint8_t clear_on_exit, uint8_t wait_all,
tick_t timeout)
{
    if (!eg) {
        return 0;
    }
#if CONFIG_ISR_API_GUARD
    if (timeout != 0 && cgrtos_reject_blocking_in_isr()) {
        return 0;
    }
#endif

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
 * @details 临界区内读取 eg->flags 快照。
 * @param[in] eg 事件组指针
 * @return 当前 flags 位图
 * @retval 非 0  当前标志（eg 有效时）
 * @retval 0     eg 为 NULL
 * @note 只读
 * @warning 并发 set/clear 时值为瞬时快照
 * @attention ✅ ISR 可读；❌ 不阻塞
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
 * @details 弹出 wait_q 全部等待者（wake_ok=0）并 unblock；清零结构体；可能 yield。
 * @param[in] eg 事件组指针
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL eg 为空
 * @note 动态池对象递减 g_eg_cnt；静态对象仅清零
 * @warning 删除后禁止再使用指针
 * @attention ❌ ISR；✅ 可能切换
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
