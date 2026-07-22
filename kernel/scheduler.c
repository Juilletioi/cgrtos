/**
 * @file scheduler.c
 * @brief CG-RTOS 多策略抢占式 SMP 调度器核心
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 调度策略：
 *   SCHED_PRIORITY  同优先级内粘性调度（无轮转）
 *   SCHED_RR        同优先级时间片轮转
 *   SCHED_CFS       按 vruntime 排序的公平队列
 *   SCHED_EDF       按 deadline 排序；通过 EDF 释放轮释放
 *   SCHED_HYBRID    prio>=RT_THRESHOLD 走 Priority，否则 CFS
 *
 * SMP：
 *   每核就绪位图 + CFS/EDF 链表；hart0 tick 上周期性加权 push 负载均衡。
 *   可选 idle 工作窃取（CONFIG_SMP_IDLE_STEAL）；默认仅 push 以保证 QEMU 稳定。
 *   ready_lock 在本地 IRQ 关闭下获取；跨核 unblock/create/affinity 向目标 hart 发 MSIP IPI。
 *
 * 关键路径：cgrtos_tick_handler → delayed/EDF/LB → yield_pending；
 *           trap_vector → cgrtos_sched_switch_from_trap。
 */

#include "cgrtos.h"
#include <string.h>

extern cgrtos_task_t    g_tasks[CONFIG_MAX_TASKS];
extern cgrtos_task_t   *g_current[CONFIG_NUM_CORES];
extern cgrtos_task_t    g_idle[CONFIG_NUM_CORES];
extern tick_t           g_ticks;
extern uint32_t         g_cs_count;
extern uint32_t         g_cs_count_core[CONFIG_NUM_CORES];
extern uint32_t         g_lb_migrate_count;
extern uint32_t         g_lb_steal_count;
extern uint8_t          g_sched_run;
extern volatile uint8_t g_yield_pending[CONFIG_NUM_CORES];
extern volatile uint32_t g_secondary_online;

#if CONFIG_USE_HOOKS
extern cgrtos_hook_fn_t g_tick_hook;
#endif

void cgrtos_timer_process_tick(void);

/** @brief 优先级就绪队列（双向链表 + 计数）。 */
typedef struct {
    cgrtos_task_t *head; /**< 队头任务。 */
    cgrtos_task_t *tail; /**< 队尾任务。 */
    uint32_t       count; /**< 队列中任务数。 */
} ready_list_t;

/** @brief 每核、每优先级的就绪队列。 */
static ready_list_t g_ready[CONFIG_NUM_CORES][CONFIG_MAX_PRIORITY + 1];
/** @brief 每核优先级就绪位图（bit=prio 有就绪任务）。 */
static uint32_t     g_ready_bitmap[CONFIG_NUM_CORES];
/** @brief 每核 CFS 就绪链表（按 vruntime 升序）。 */
static list_t       g_cfs_ready[CONFIG_NUM_CORES];
/**
 * @brief 全局 MC-EDF 就绪链表（按 deadline 升序）
 * @note 非每核队列；sched_pick_edf 按 Global EDF 将前 m 个任务分配到 m 个在线核。
 */
static list_t       g_edf_global;
#if CONFIG_USE_EDF && CONFIG_USE_EDF_HEAP
static cgrtos_task_t *g_edf_heap[CONFIG_MAX_TASKS];
static uint32_t       g_edf_heap_n;
#endif
/** @brief 全局延迟唤醒链表（按 wake_tick 排序）。 */
static list_t       g_delayed_list;
/** @brief EDF 释放时间轮各槽位链表。 */
static list_t       g_edf_rel[CONFIG_EDF_RELEASE_SLOTS];
/** @brief EDF 释放轮当前游标槽位。 */
static uint32_t     g_edf_rel_cursor;
/** @brief 调度器挂起嵌套计数（>0 时不切换）。 */
static uint32_t     g_sched_suspend_count;
/** @brief 保护就绪队列与延迟/EDF 结构的自旋锁。 */
static spinlock_t   g_ready_lock;
/** @brief hart0 负载均衡 tick 计数器。 */
static uint32_t     g_lb_tick;
/** @brief 每核强制让出标志（ecall yield 等）。 */
static volatile uint8_t g_force_yield[CONFIG_NUM_CORES];
#if CONFIG_SCHED_STATS
static tick_t g_sched_max_latency;
static uint32_t g_sched_latency_samples;
#endif
#if CONFIG_USE_EDF
/** @brief 持 g_klock 时推迟 MC-EDF kick，避免 IPI 对端再争 g_klock 死锁 */
static volatile uint8_t g_edf_kick_pending;
#endif

/**
 * @brief 从就绪位图取最高优先级数值
 * @details bitmap 为 0 返回 -1；否则用 __builtin_clz 计算最高置位，31-clz 即优先级。
 * @param[in] bitmap 优先级位图（bit i 表示优先级 i 有就绪任务）
 * @return 最高优先级 [0..31]；位图为 0 时 -1
 * @retval >=0 最高优先级
 * @retval -1 无就绪任务
 * @note 内联热路径；调用方已持锁或 IRQ 关闭
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static inline int sched_highest_ready_prio(uint32_t bitmap)
{
    if (!bitmap) {
        return -1;
    }
    return (int)(31 - __builtin_clz(bitmap));
}

/**
 * @brief 判断任务是否走优先级/RR 就绪队列
 * @details SCHED_PRIORITY/SCHED_RR 直接返回 1；HYBRID 且 prio>=RT_THRESHOLD 返回 1；否则 0。
 * @param[in] task 任务 TCB；NULL 返回 0
 * @return 非零表示使用优先级队列
 * @retval 1 走优先级队列
 * @retval 0 不使用
 * @note 策略路由辅助
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static inline int sched_uses_priority(cgrtos_task_t *task)
{
    if (!task) {
        return 0;
    }
    if (task->policy == SCHED_PRIORITY || task->policy == SCHED_RR) {
        return 1;
    }
    if (task->policy == SCHED_HYBRID) {
        return task->prio >= CONFIG_RT_PRIO_THRESHOLD;
    }
    return 0;
}

/**
 * @brief 判断任务是否走 CFS 就绪队列
 * @details SCHED_CFS 返回 1；HYBRID 且 prio<RT_THRESHOLD 返回 1；否则 0。
 * @param[in] task 任务 TCB；NULL 返回 0
 * @return 非零表示使用 CFS 队列
 * @retval 1 走 CFS
 * @retval 0 不使用
 * @note 策略路由辅助
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static inline int sched_uses_cfs(cgrtos_task_t *task)
{
    if (!task) {
        return 0;
    }
    if (task->policy == SCHED_CFS) {
        return 1;
    }
    if (task->policy == SCHED_HYBRID) {
        return task->prio < CONFIG_RT_PRIO_THRESHOLD;
    }
    return 0;
}

/**
 * @brief 判断任务是否走 EDF 就绪队列
 * @details CONFIG_USE_EDF 且 policy==SCHED_EDF 时返回 1；否则 0。
 * @param[in] task 任务 TCB
 * @return 非零表示 EDF 策略
 * @retval 1 EDF
 * @retval 0 非 EDF
 * @note 未启用 EDF 时编译为恒 0
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static inline int sched_uses_edf(cgrtos_task_t *task)
{
#if CONFIG_USE_EDF
    return task && task->policy == SCHED_EDF;
#else
    (void)task;
    return 0;
#endif
}

/**
 * @brief EDF 任务是否允许在指定核运行（硬亲和检查）
 * @details cpu_aff==0xFF 任意核可运行；否则须 cpu_aff==cpu。
 * @param[in] task 任务 TCB
 * @param[in] cpu 目标 hart 编号
 * @return 非零表示可在该核运行
 * @retval 1 亲和允许
 * @retval 0 不允许或 task 为空
 * @note MC-EDF 分配与 ready_count 统计共用
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static int sched_edf_affinity_ok(cgrtos_task_t *task, uint8_t cpu)
{
    if (!task) {
        return 0;
    }
    return (task->cpu_aff == 0xFF) || (task->cpu_aff == cpu);
}

/**
 * @brief MC-EDF：请求所有在线核重新评估调度
 * @details 对本核置 g_yield_pending；对其余在线核置 pending 并发送 MSIP IPI。
 * @return 无
 * @retval 无
 * @note EDF 入队/释放后调用；使更早 deadline 能抢占
 * @warning 调用方不得持有 g_ready_lock（IPI 路径可能间接争用）
 * @attention @internal ❌ ISR；❌ block/switch
 */
static void sched_mcedf_kick_all(void)
{
    uint8_t here = (uint8_t)read_csr(mhartid);
    g_yield_pending[here] = 1;
#if CONFIG_NUM_CORES > 1
    for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
        if (c == here) {
            continue;
        }
        if (c != 0 && !CGRTOS_CORE_ONLINE(c)) {
            continue;
        }
        g_yield_pending[c] = 1;
        cgrtos_smp_send_ipi(c);
    }
#endif
}

/**
 * @brief 请求 MC-EDF 重评估：临界区/ISR 内仅置 pending
 * @details 若在临界区或 ISR：置 g_edf_kick_pending 与本核 yield；否则立即 sched_mcedf_kick_all。
 * @return 无
 * @retval 无
 * @note 持 g_klock 时同步 IPI 可能导致对端 enter_critical 自旋死锁
 * @warning 持 g_klock 时禁止同步全员 IPI
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_edf_kick_request(void)
{
#if CONFIG_USE_EDF
    if (cgrtos_in_critical() || cgrtos_in_isr()) {
        /* 临界区/ISR：只记 pending 与本核 yield；出临界区或任务态再 flush */
        g_edf_kick_pending = 1;
        g_yield_pending[(uint8_t)read_csr(mhartid)] = 1;
        return;
    }
    g_edf_kick_pending = 0;
    sched_mcedf_kick_all();
#else
    (void)0;
#endif
}

/**
 * @brief 刷新推迟的 MC-EDF kick
 * @details exit_critical 最外层调用；若 pending 且仍在 ISR 则仅置各核 yield，任务态则 sched_mcedf_kick_all。
 * @return 无
 * @retval 无
 * @note 与 sched_edf_kick_request 配对，避免临界区内 IPI 死锁
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_sched_edf_kick_flush(void)
{
#if CONFIG_USE_EDF
    if (!g_edf_kick_pending) {
        return;
    }
    g_edf_kick_pending = 0;
    /* tick/ISR 里已在 exit_critical：只置 yield，避免在 ISR 尾再打全员 IPI 放大抖动 */
    if (cgrtos_in_isr()) {
        uint8_t here = (uint8_t)read_csr(mhartid);
        g_yield_pending[here] = 1;
#if CONFIG_NUM_CORES > 1
        for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
            if (c != here && (c == 0 || CGRTOS_CORE_ONLINE(c))) {
                g_yield_pending[c] = 1;
            }
        }
#endif
        return;
    }
    sched_mcedf_kick_all();
#endif
}

/**
 * @brief 从全局 MC-EDF 就绪结构移除任务
 * @details 堆模式：线性查找后 sift；链表模式：list_remove g_edf_global。调用方已持 g_ready_lock。
 * @param[in] task EDF 任务 TCB
 * @return 无
 * @retval 无
 * @note remove_ready_locked 内部路径
 * @warning task 为空直接返回
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_edf_global_remove(cgrtos_task_t *task)
{
    if (!task) {
        return;
    }
#if CONFIG_USE_EDF && CONFIG_USE_EDF_HEAP
    {
        uint32_t i, j;
        for (i = 0; i < g_edf_heap_n; i++) {
            if (g_edf_heap[i] == task) {
                g_edf_heap_n--;
                g_edf_heap[i] = g_edf_heap[g_edf_heap_n];
                g_edf_heap[g_edf_heap_n] = 0;
                /* sift down/up */
                j = i;
                while (1) {
                    uint32_t l = j * 2 + 1, r = l + 1, best = j;
                    if (l < g_edf_heap_n &&
                        g_edf_heap[l]->deadline < g_edf_heap[best]->deadline) {
                        best = l;
                    }
                    if (r < g_edf_heap_n &&
                        g_edf_heap[r]->deadline < g_edf_heap[best]->deadline) {
                        best = r;
                    }
                    if (best == j) {
                        break;
                    }
                    {
                        cgrtos_task_t *tmp = g_edf_heap[j];
                        g_edf_heap[j] = g_edf_heap[best];
                        g_edf_heap[best] = tmp;
                    }
                    j = best;
                }
                while (j > 0) {
                    uint32_t p = (j - 1) / 2;
                    if (g_edf_heap[j]->deadline >= g_edf_heap[p]->deadline) {
                        break;
                    }
                    {
                        cgrtos_task_t *tmp = g_edf_heap[j];
                        g_edf_heap[j] = g_edf_heap[p];
                        g_edf_heap[p] = tmp;
                    }
                    j = p;
                }
                return;
            }
        }
    }
#else
    if (task->edf_item.next || task->edf_item.prev ||
        g_edf_global.head == &task->edf_item) {
        list_remove(&g_edf_global, &task->edf_item);
    }
#endif
}

/**
 * @brief 将 EDF 任务按 deadline 插入全局 MC-EDF 就绪结构
 * @details 先 sched_edf_global_remove 防重复；堆模式 sift-up 插入，链表模式 list_insert_sorted_asc。
 * @param[in] task EDF 任务 TCB
 * @return 无
 * @retval 无
 * @note 调用方已持 g_ready_lock
 * @warning 堆满时静默丢弃入队
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_add_edf_ready(cgrtos_task_t *task)
{
    sched_edf_global_remove(task);
#if CONFIG_USE_EDF && CONFIG_USE_EDF_HEAP
    if (g_edf_heap_n >= CONFIG_MAX_TASKS) {
        return;
    }
    {
        uint32_t i = g_edf_heap_n++;
        g_edf_heap[i] = task;
        while (i > 0) {
            uint32_t p = (i - 1) / 2;
            if (g_edf_heap[i]->deadline >= g_edf_heap[p]->deadline) {
                break;
            }
            {
                cgrtos_task_t *tmp = g_edf_heap[i];
                g_edf_heap[i] = g_edf_heap[p];
                g_edf_heap[p] = tmp;
            }
            i = p;
        }
    }
#else
    list_insert_sorted_asc(&g_edf_global, &task->edf_item, task->deadline);
#endif
}

/**
 * @brief 从 EDF 释放时间轮移除任务
 * @details 由 edf_rel_item.value 低 32 位算槽位；在链上则 list_remove；清除 edf_on_wheel。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 调用方应已持有 g_ready_lock
 * @warning 未在轮上则 no-op
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void edf_wheel_remove(cgrtos_task_t *task)
{
    /* 1. task 为空或未在轮上则直接返回 */
    if (!task || !task->edf_on_wheel) {
        return;
    }
    /* 2. 从 edf_rel_item.value 低 32 位计算所在槽位 */
    uint32_t slot = (uint32_t)(task->edf_rel_item.value &
                               (CONFIG_EDF_RELEASE_SLOTS - 1U));
    /* 3. 若节点确实在链表中，调用 list_remove 移除 */
    if (task->edf_rel_item.next || task->edf_rel_item.prev ||
        g_edf_rel[slot].head == &task->edf_rel_item) {
        list_remove(&g_edf_rel[slot], &task->edf_rel_item);
    }
    /* 4. 清除 edf_on_wheel 标志 */
    task->edf_on_wheel = 0;
}

/**
 * @brief 将 EDF 任务挂入释放时间轮
 * @details 关 IRQ 持 ready_lock；移除旧轮条目；非 EDF 或 period==0 则返回；否则按 deadline 算 slot/rounds 插入并置 edf_on_wheel。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 内部获取 ready_lock；释放时刻 when=max(deadline,g_ticks)
 * @warning task 为空直接返回
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_sched_edf_arm(cgrtos_task_t *task)
{
    if (!task) {
        return;
    }

    /* 1. 关 IRQ 并获取 g_ready_lock */
    uint64_t flags = cgrtos_irq_save();
    cgrtos_spin_lock(&g_ready_lock);
    /* 2. 先从时间轮移除 task 的旧条目（若有） */
    edf_wheel_remove(task);

    /* 3. 若非 EDF 或 period==0，解锁返回（无需挂轮） */
    if (task->policy != SCHED_EDF || task->period == 0) {
        cgrtos_spin_unlock(&g_ready_lock);
        cgrtos_irq_restore(flags);
        return;
    }

    /* 4. 计算释放时刻 when = max(deadline, g_ticks) */
    tick_t when = task->deadline;
    if (when < g_ticks) {
        when = g_ticks;
    }
    /* 5. 计算 delta、slot 与 rounds */
    uint32_t delta = (uint32_t)(when - g_ticks);
    uint32_t slot = (g_edf_rel_cursor + delta) & (CONFIG_EDF_RELEASE_SLOTS - 1U);
    uint32_t rounds = delta / CONFIG_EDF_RELEASE_SLOTS;

    /* 6. 将 rounds/slot 写入 value 并插入对应槽位链表 */
    task->edf_rel_item.value = ((uint64_t)rounds << 32) | slot;
    list_insert_end(&g_edf_rel[slot], &task->edf_rel_item);
    /* 7. 置 edf_on_wheel=1，解锁并恢复 IRQ */
    task->edf_on_wheel = 1;
    cgrtos_spin_unlock(&g_ready_lock);
    cgrtos_irq_restore(flags);
}

/**
 * @brief EDF 周期释放：推进 deadline 并唤醒/入就绪队列
 * @details deadline+=period；BLOCKED+DELAY 则 unblock；READY 则 remove+add；其它非 RUNNING/SUSPENDED/DELETED 则 add_ready；最后 edf_arm。
 * @param[in] task EDF 任务 TCB
 * @return 无
 * @retval 无
 * @note 由 sched_process_edf_releases 在锁外调用
 * @warning 非 EDF 或 period==0 直接返回
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_edf_release(cgrtos_task_t *task)
{
    /* 1. 校验 task 为 EDF 且 period != 0 */
    if (!task || task->policy != SCHED_EDF || task->period == 0) {
        return;
    }
    /* 2. 将 deadline 推进到 g_ticks + period（新周期） */
    task->deadline = g_ticks + task->period;
    /* 3. 按当前 state 分支处理 */
    if (task->state == TASK_BLOCKED && task->block_reason == BLOCK_DELAY) {
        /* 3a. BLOCKED + BLOCK_DELAY：调用 unblock 唤醒 */
        cgrtos_sched_unblock(task);
    } else if (task->state == TASK_READY) {
        /* 3b. READY：先从就绪队列移除再重新插入（按新 deadline 排序） */
        cgrtos_sched_remove_ready(task);
        cgrtos_sched_add_ready(task);
    } else if (task->state != TASK_RUNNING &&
               task->state != TASK_SUSPENDED && task->state != TASK_DELETED) {
        /* 3c. 非 RUNNING/SUSPENDED/DELETED：直接 add_ready */
        cgrtos_sched_add_ready(task);
    }
    /* 4. 调用 cgrtos_sched_edf_arm 重新挂入释放轮 */
    cgrtos_sched_edf_arm(task);
}

/**
 * @brief 处理当前 EDF 释放轮槽位到期任务
 * @details 持锁推进 cursor；遍历槽位递减 rounds；rounds==0 则收集；锁外 sched_edf_release；有释放则 kick_request。
 * @return 无
 * @retval 无
 * @note hart0 tick 调用；多轮 delta 由高 32 位 rounds 表示
 * @warning 单次最多收集 16 个任务
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_process_edf_releases(void)
{
    cgrtos_task_t *to_release[16];
    uint32_t n = 0;
    /* 1. 关 IRQ 并获取 g_ready_lock */
    uint64_t flags = cgrtos_irq_save();
    cgrtos_spin_lock(&g_ready_lock);

    /* 2. 游标 g_edf_rel_cursor 前进一步（模 slots） */
    g_edf_rel_cursor = (g_edf_rel_cursor + 1U) & (CONFIG_EDF_RELEASE_SLOTS - 1U);
    list_t *slot = &g_edf_rel[g_edf_rel_cursor];
    list_item_t *item = slot->head;

    /* 3. 遍历当前槽位链表 */
    while (item) {
        list_item_t *next = item->next;
        uint32_t rounds = (uint32_t)(item->value >> 32);
        if (rounds > 0) {
            /* 3a. 若 rounds > 0，递减 rounds 并跳过（尚未到期） */
            item->value = ((uint64_t)(rounds - 1U) << 32) |
                          (item->value & 0xFFFFFFFFULL);
            item = next;
            continue;
        }

        /* 3b. 若 rounds == 0，从链表移除，收集到 to_release[] */
        list_remove(slot, item);
        cgrtos_task_t *task = (cgrtos_task_t *)((uint8_t *)item -
                                                offsetof(cgrtos_task_t, edf_rel_item));
        task->edf_on_wheel = 0;
        if (n < 16U) {
            to_release[n++] = task;
        }
        item = next;
    }

    /* 4. 解锁并恢复 IRQ */
    cgrtos_spin_unlock(&g_ready_lock);
    cgrtos_irq_restore(flags);

    /* 5. 在锁外逐个调用 sched_edf_release 执行实际释放 */
    for (uint32_t i = 0; i < n; i++) {
        sched_edf_release(to_release[i]);
    }
    /* 6. 有释放则请求 MC-EDF 重选（ISR 内不持 g_klock，可立即 kick） */
    if (n > 0) {
        sched_edf_kick_request();
    }
}

/**
 * @brief 计算任务应运行的目标 CPU
 * @details 硬亲和 cpu_aff!=0xFF 时校验在线后返回；否则 run_cpu 有效且在线则返回；默认 0。
 * @param[in] task 任务 TCB
 * @return 目标 hart 编号
 * @retval 0..N-1 目标核
 * @retval 0 task 为空
 * @note 就绪入队、unblock IPI、负载统计共用
 * @warning 次核未上线时回退 hart0
 * @attention ✅ ISR；❌ block/switch
 */
uint8_t cgrtos_sched_target_core(cgrtos_task_t *task)
{
    if (!task) {
        return 0;
    }
    if (task->cpu_aff != 0xFF) {
        if (task->cpu_aff >= CONFIG_NUM_CORES ||
            (task->cpu_aff != 0 && !CGRTOS_CORE_ONLINE(task->cpu_aff))) {
            return 0;
        }
        return task->cpu_aff;
    }
    if (task->run_cpu < CONFIG_NUM_CORES) {
        if (task->run_cpu != 0 && !CGRTOS_CORE_ONLINE(task->run_cpu)) {
            return 0;
        }
        return task->run_cpu;
    }
    return 0;
}

/**
 * @brief 将任务加入指定核的优先级就绪队列
 * @details 按 target_core 与 prio 追加到 g_ready[core][prio] 尾部；置 bitmap 对应位。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 调用方应已持有 g_ready_lock
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_add_priority_ready(cgrtos_task_t *task)
{
    /* 1. 根据 task 计算目标核 core 与优先级 prio */
    uint8_t core = cgrtos_sched_target_core(task);
    uint8_t prio = task->prio;
    ready_list_t *list = &g_ready[core][prio];

    /* 2. 将 task 追加到 g_ready[core][prio] 双向链表尾部 */
    task->next = 0;
    task->prev = list->tail;
    if (list->tail) {
        list->tail->next = task;
    } else {
        list->head = task;
    }
    list->tail = task;
    list->count++;
    /* 3. 置 g_ready_bitmap[core] 对应 prio 位 */
    g_ready_bitmap[core] |= (1U << prio);
}

/**
 * @brief 将任务按 vruntime 插入 CFS 就绪队列
 * @details 计算 target_core；list_insert_sorted_asc 到 g_cfs_ready[core]。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 调用方应已持有 g_ready_lock
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_add_cfs_ready(cgrtos_task_t *task)
{
    /* 1. 计算任务目标核 core */
    uint8_t core = cgrtos_sched_target_core(task);
    /* 2. 按 vruntime 升序插入 g_cfs_ready[core] */
    list_insert_sorted_asc(&g_cfs_ready[core], &task->cfs_item, task->vruntime);
}

/**
 * @brief 从优先级就绪队列移除任务（前向声明）
 * @details 实现见本文件 sched_remove_priority_ready 定义；供 sched_add_ready_locked 前向引用。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 前向声明
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_remove_priority_ready(cgrtos_task_t *task);

/**
 * @brief 将任务加入对应策略就绪队列（调用方已持 g_ready_lock）
 * @details 按 EDF/CFS/优先级分支入队；初始化 slice_remain；state=TASK_READY；可选记录 ready_since。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 不获取锁；与 remove_ready_locked 配对
 * @warning DELETED 或 NULL 直接返回
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_add_ready_locked(cgrtos_task_t *task)
{
    /* 1. task 为空或已 DELETED 则返回 */
    if (!task || task->state == TASK_DELETED) {
        return;
    }

    /* 2. 按策略分支入对应就绪队列 */
    if (sched_uses_edf(task)) {
        sched_add_edf_ready(task);
    } else if (sched_uses_cfs(task)) {
        sched_add_cfs_ready(task);
    } else {
        sched_add_priority_ready(task);
    }

    /* 3. 若 slice_remain 为 0，按策略初始化时间片 */
    if (task->slice_remain == 0) {
        if (sched_uses_cfs(task)) {
            task->slice_remain = CONFIG_CFS_SLICE_TICKS;
        } else {
            task->slice_remain = CONFIG_TIME_SLICE_TICKS;
        }
    }
    /* 4. 将 task->state 置为 TASK_READY */
    task->state = TASK_READY;
#if CONFIG_SCHED_STATS
    task->ready_since = g_ticks;
#endif
}

/**
 * @brief 从就绪队列移除任务（调用方已持 g_ready_lock）
 * @details 非 TASK_READY 返回；按策略从 EDF 全局/CFS 链/优先级链移除。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 不获取锁
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_remove_ready_locked(cgrtos_task_t *task)
{
    /* 1. task 为空或 state != TASK_READY 则返回 */
    if (!task || task->state != TASK_READY) {
        return;
    }

    /* 2. 按策略从对应队列移除 */
    if (sched_uses_edf(task)) {
        sched_edf_global_remove(task);
    } else if (sched_uses_cfs(task)) {
        if (task->cfs_item.next || task->cfs_item.prev ||
            g_cfs_ready[cgrtos_sched_target_core(task)].head == &task->cfs_item) {
            list_remove(&g_cfs_ready[cgrtos_sched_target_core(task)], &task->cfs_item);
        }
    } else {
        sched_remove_priority_ready(task);
    }
}

/**
 * @brief 将任务加入对应策略的就绪队列
 * @details 关 IRQ 持 ready_lock；sched_add_ready_locked；解锁；EDF 任务则 sched_edf_kick_request。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 防止 timer/IPI 重入死锁
 * @warning DELETED 或 NULL 直接返回
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_sched_add_ready(cgrtos_task_t *task)
{
    /* 1. task 为空或已 DELETED 则返回 */
    if (!task || task->state == TASK_DELETED) {
        return;
    }

    /* 2. 关 IRQ 并获取 g_ready_lock */
    uint64_t flags = cgrtos_irq_save();
    cgrtos_spin_lock(&g_ready_lock);
    /* 3. 调用 sched_add_ready_locked 执行实际入队 */
    int is_edf = sched_uses_edf(task);
    sched_add_ready_locked(task);
    /* 4. 解锁并恢复 IRQ */
    cgrtos_spin_unlock(&g_ready_lock);
    cgrtos_irq_restore(flags);
    /* 5. EDF 入队后请求踢核；持 g_klock 时推迟到 exit_critical */
    if (is_edf) {
        sched_edf_kick_request();
    }
}

/**
 * @brief 从优先级就绪队列移除任务
 * @details 更新双向链表 head/tail；count--；队列空则清 bitmap 位。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note remove_ready_locked 内部路径
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_remove_priority_ready(cgrtos_task_t *task)
{
    /* 1. 定位 task 所在核 core 与优先级 prio 的 ready_list */
    uint8_t core = cgrtos_sched_target_core(task);
    uint8_t prio = task->prio;
    ready_list_t *list = &g_ready[core][prio];

    /* 2. 更新双向链表 prev/next 指针，维护 head/tail */
    if (task->prev) {
        task->prev->next = task->next;
    } else {
        list->head = task->next;
    }
    if (task->next) {
        task->next->prev = task->prev;
    } else {
        list->tail = task->prev;
    }
    /* 3. 清空 task 的 next/prev，count 递减 */
    task->next = 0;
    task->prev = 0;

    if (list->count > 0) {
        list->count--;
    }
    /* 4. 若该优先级队列变空，清除 g_ready_bitmap 对应位 */
    if (!list->count) {
        g_ready_bitmap[core] &= ~(1U << prio);
    }
}

/**
 * @brief 从就绪队列移除任务
 * @details 非 TASK_READY 返回；关 IRQ 持 ready_lock；sched_remove_ready_locked；解锁恢复 IRQ。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 仅 TASK_READY 有效
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_sched_remove_ready(cgrtos_task_t *task)
{
    /* 1. task 为空或 state != TASK_READY 则返回 */
    if (!task || task->state != TASK_READY) {
        return;
    }

    /* 2. 关 IRQ 并获取 g_ready_lock */
    uint64_t flags = cgrtos_irq_save();
    cgrtos_spin_lock(&g_ready_lock);
    /* 3. 调用 sched_remove_ready_locked 执行实际出队 */
    sched_remove_ready_locked(task);
    /* 4. 解锁并恢复 IRQ */
    cgrtos_spin_unlock(&g_ready_lock);
    cgrtos_irq_restore(flags);
}

/**
 * @brief 在指定核上选取最高优先级且亲和匹配的就绪任务
 * @details 从 bitmap 取最高 prio；遍历该链跳过 cpu_aff 不匹配；返回首个匹配或 NULL。
 * @param[in] cpu 核编号
 * @return 任务指针；无可用时 NULL
 * @retval 非 NULL 选中任务
 * @retval NULL 无匹配
 * @note switch_from_trap 内部；持 g_ready_lock
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static cgrtos_task_t *sched_pick_priority(uint8_t cpu)
{
    int prio = sched_highest_ready_prio(g_ready_bitmap[cpu]);
    if (prio < 0) {
        return 0;
    }

    ready_list_t *list = &g_ready[cpu][prio];
    cgrtos_task_t *task = list->head;
    while (task) {
        if (task->cpu_aff == 0xFF || task->cpu_aff == cpu) {
            return task;
        }
        task = task->next;
    }
    return 0;
}

/**
 * @brief 在指定核上选取 vruntime 最小的 CFS 任务
 * @details peek g_cfs_ready[cpu] 队头；offsetof 反算 task 指针。
 * @param[in] cpu 核编号
 * @return 任务指针；队空 NULL
 * @retval 非 NULL CFS 队头
 * @retval NULL 队空
 * @note switch_from_trap 内部
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static cgrtos_task_t *sched_pick_cfs(uint8_t cpu)
{
    list_item_t *item = list_peek_head(&g_cfs_ready[cpu]);
    if (!item) {
        return 0;
    }
    return (cgrtos_task_t *)((uint8_t *)item - offsetof(cgrtos_task_t, cfs_item));
}

/**
 * @brief MC-EDF：为本核分配应运行的 EDF 任务
 * @details 枚举在线核；按 deadline 升序填充至多 m 席；硬亲和占固定位，软亲和优先 run_cpu；返回 assigned[cpu]。
 * @param[in] cpu 核编号
 * @return 分配给本核的 EDF 任务；无则 NULL
 * @retval 非 NULL 分配任务
 * @retval NULL 无 EDF 或无席
 * @note RUNNING 不在就绪链；Global EDF 规则
 * @warning cpu 越界返回 NULL
 * @attention @internal ✅ ISR；❌ block/switch
 */
static cgrtos_task_t *sched_pick_edf(uint8_t cpu)
{
#if !CONFIG_USE_EDF
    (void)cpu;
    return 0;
#else
    uint8_t cores[CONFIG_MAX_CORES];
    cgrtos_task_t *assigned[CONFIG_MAX_CORES];
    int n = 0;
    int filled = 0;
    int i;
#if !CONFIG_USE_EDF_HEAP
    list_item_t *it;
#endif

    if (cpu >= CONFIG_NUM_CORES) {
        return 0;
    }

    for (i = 0; i < CONFIG_MAX_CORES; i++) {
        assigned[i] = 0;
    }

    /* 1. 在线核列表（与 sched_mcedf_online_cores 一致） */
    n = 0;
    cores[n++] = 0;
#if CONFIG_NUM_CORES > 1
    for (uint8_t c = 1; c < CONFIG_NUM_CORES; c++) {
        if (CGRTOS_CORE_ONLINE(c)) {
            cores[n++] = c;
        }
    }
#endif

    /* 2-5. 按 deadline 填充至多 n 席 */
#if CONFIG_USE_EDF_HEAP
    /* 从堆中按 deadline 选前 n（选择排序式扫描，n<=cores 很小） */
    {
        uint8_t taken[CONFIG_MAX_TASKS];
        uint32_t k;
        memset(taken, 0, sizeof(taken));
        while (filled < n) {
            cgrtos_task_t *best = 0;
            uint32_t best_i = 0;
            for (k = 0; k < g_edf_heap_n; k++) {
                if (taken[k]) {
                    continue;
                }
                if (!best || g_edf_heap[k]->deadline < best->deadline) {
                    best = g_edf_heap[k];
                    best_i = k;
                }
            }
            if (!best) {
                break;
            }
            taken[best_i] = 1;
            {
                cgrtos_task_t *t = best;
                if (t->cpu_aff != 0xFF) {
                    uint8_t a = t->cpu_aff;
                    if (a < CONFIG_NUM_CORES && !assigned[a] &&
                        sched_edf_affinity_ok(t, a) &&
                        (a == 0 || CGRTOS_CORE_ONLINE(a))) {
                        assigned[a] = t;
                        filled++;
                    }
                    continue;
                }
                {
                    uint8_t prefer = t->run_cpu;
                    if (prefer < CONFIG_NUM_CORES && !assigned[prefer] &&
                        (prefer == 0 || CGRTOS_CORE_ONLINE(prefer))) {
                        assigned[prefer] = t;
                        filled++;
                        continue;
                    }
                }
                for (i = 0; i < n; i++) {
                    uint8_t c = cores[i];
                    if (!assigned[c]) {
                        assigned[c] = t;
                        filled++;
                        break;
                    }
                }
            }
        }
    }
#else
    for (it = g_edf_global.head; it && filled < n; it = it->next) {
        cgrtos_task_t *t = (cgrtos_task_t *)((uint8_t *)it -
                                             offsetof(cgrtos_task_t, edf_item));
        if (t->cpu_aff != 0xFF) {
            uint8_t a = t->cpu_aff;
            if (a < CONFIG_NUM_CORES && !assigned[a] &&
                sched_edf_affinity_ok(t, a) &&
                (a == 0 || CGRTOS_CORE_ONLINE(a))) {
                assigned[a] = t;
                filled++;
            }
            continue;
        }

        /* 软亲和：先试 run_cpu */
        {
            uint8_t prefer = t->run_cpu;
            if (prefer < CONFIG_NUM_CORES && !assigned[prefer] &&
                (prefer == 0 || CGRTOS_CORE_ONLINE(prefer))) {
                assigned[prefer] = t;
                filled++;
                continue;
            }
        }
        for (i = 0; i < n; i++) {
            uint8_t c = cores[i];
            if (!assigned[c]) {
                assigned[c] = t;
                filled++;
                break;
            }
        }
    }
#endif /* CONFIG_USE_EDF_HEAP */

    /* 6. */
    return assigned[cpu];
#endif /* CONFIG_USE_EDF */
}

/**
 * @brief 综合 MC-EDF/优先级/CFS 选取下一运行任务
 * @details 取 edf/pri/cfs；EDF 在 slack 窗口内或无 RT 候选则选 EDF；否则 pri；否则 cfs；否则 idle。
 * @param[in] cpu 核编号
 * @return 下一任务 TCB
 * @retval 非 idle 任务
 * @retval idle 无其它就绪
 * @note switch_from_trap 核心选择
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static cgrtos_task_t *sched_pick_next(uint8_t cpu)
{
    cgrtos_task_t *edf = sched_pick_edf(cpu);
    cgrtos_task_t *pri = sched_pick_priority(cpu);
    cgrtos_task_t *cfs = sched_pick_cfs(cpu);

    if (edf) {
        if (!pri || edf->deadline <= g_ticks + CONFIG_MCEDF_PRIO_SLACK_TICKS) {
            return edf;
        }
    }
    if (pri) {
        return pri;
    }
    if (cfs) {
        return cfs;
    }
    return &g_idle[cpu];
}

/**
 * @brief 时间片耗尽时刷新 slice_remain
 * @details CFS 用 CFS_SLICE；其余 TIME_SLICE；不重入就绪队列（RUNNING 仍运行）。
 * @param[in] task 当前运行任务
 * @return 无
 * @retval 无
 * @note 实际 requeue 在 switch_from_trap
 * @warning idle(id==0) 跳过
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_rotate_time_slice(cgrtos_task_t *task)
{
    if (!task || task->id == 0) {
        return;
    }

    if (sched_uses_cfs(task)) {
        task->slice_remain = CONFIG_CFS_SLICE_TICKS;
    } else {
        task->slice_remain = CONFIG_TIME_SLICE_TICKS;
    }
}

/**
 * @brief 将任务按优先级插入 IPC 等待队列（兼容封装）
 * @details 委托 cgrtos_wait_list_add_priority 按优先级降序插入。
 * @param[inout] head 等待队列头指针
 * @param[in] task 待插入任务
 * @return 无
 * @retval 无
 * @note IPC 阻塞路径使用
 * @warning head 或 task 须有效
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_wait_list_add(cgrtos_task_t *volatile *head, cgrtos_task_t *task)
{
    cgrtos_wait_list_add_priority(head, task);
}

/**
 * @brief 将任务按优先级降序插入等待队列
 * @details 从头遍历跳过 prio>=task->prio；在 cur 前插入并维护双向链；队头则更新 *head。
 * @param[inout] head 等待队列头指针
 * @param[in] task 待插入任务
 * @return 无
 * @retval 无
 * @note 高优先级在队头
 * @warning 须持 IPC 锁或临界区
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_wait_list_add_priority(cgrtos_task_t *volatile *head, cgrtos_task_t *task)
{
    cgrtos_task_t *cur = *head;
    cgrtos_task_t *prev = 0;

    /* 1. 从头遍历等待队列，跳过 prio >= task->prio 的节点 */
    while (cur && cur->prio >= task->prio) {
        prev = cur;
        cur = cur->next;
    }

    /* 2. 在 cur 前插入 task，维护双向链表 prev/next */
    task->next = cur;
    task->prev = prev;
    if (cur) {
        cur->prev = task;
    }
    if (prev) {
        prev->next = task;
    } else {
        /* 3. 若插入队头，更新 *head */
        *head = task;
    }
}

/**
 * @brief 从等待队列移除指定任务
 * @details 更新 prev/next；队头则更新 *head；清空 task next/prev。
 * @param[inout] head 等待队列头指针
 * @param[in] task 待移除任务
 * @return 无
 * @retval 无
 * @note purge/unblock 共用
 * @warning head 或 task 为空则返回
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_wait_list_remove(cgrtos_task_t *volatile *head, cgrtos_task_t *task)
{
    /* 1. head 或 task 为空则返回 */
    if (!head || !task) {
        return;
    }

    /* 2. 更新 prev/next 指针；若 task 是队头则更新 *head */
    if (task->prev) {
        task->prev->next = task->next;
    } else if (*head == task) {
        *head = task->next;
    }
    if (task->next) {
        task->next->prev = task->prev;
    }
    /* 3. 清空 task 的 next/prev */
    task->next = 0;
    task->prev = 0;
}

/**
 * @brief 弹出等待队列中优先级最高的任务
 * @details 取 *head（降序维护）；cgrtos_wait_list_remove 移除并返回。
 * @param[inout] head 等待队列头指针
 * @return 队头任务；队空 NULL
 * @retval 非 NULL 最高优先级任务
 * @retval NULL 队空
 * @note IPC 唤醒路径
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
cgrtos_task_t *cgrtos_wait_list_pop_highest(cgrtos_task_t *volatile *head)
{
    /* 1. 取 *head 作为最高优先级任务（队列按 prio 降序维护） */
    cgrtos_task_t *best = *head;
    if (!best) {
        return 0;
    }

    /* 2. 调用 cgrtos_wait_list_remove 将其从队列移除 */
    cgrtos_wait_list_remove(head, best);
    /* 3. 返回该任务指针 */
    return best;
}

/**
 * @brief 从全局延迟链表移除任务
 * @details 若 delayed_item 在 g_delayed_list 则 list_remove。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note block/unblock/purge 共用
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_delayed_remove(cgrtos_task_t *task)
{
    /* 1. 若 delayed_item 在 g_delayed_list 中，调用 list_remove 移除 */
    if (task->delayed_item.next || task->delayed_item.prev ||
        g_delayed_list.head == &task->delayed_item) {
        list_remove(&g_delayed_list, &task->delayed_item);
    }
}

/**
 * @brief 阻塞任务从 IPC/事件等待队列及延迟链清除
 * @details 非 BLOCKED 返回；按 block_reason 定位 wait_q 并 remove；sched_delayed_remove。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 超时/删除/强制唤醒前调用
 * @warning 仅 TASK_BLOCKED 处理
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_task_purge_waits(cgrtos_task_t *task)
{
    if (!task || task->state != TASK_BLOCKED) {
        return;
    }

    cgrtos_task_t *volatile *head = 0;
    if (task->block_reason == BLOCK_SEM) {
        head = &((cgrtos_sem_t *)task->block_obj)->wait_q;
    } else if (task->block_reason == BLOCK_MUTEX) {
        head = &((cgrtos_mutex_t *)task->block_obj)->wait_q;
    } else if (task->block_reason == BLOCK_QUEUE_SEND) {
        head = &((cgrtos_queue_t *)task->block_obj)->send_wait_q;
    } else if (task->block_reason == BLOCK_QUEUE_RECV) {
        head = &((cgrtos_queue_t *)task->block_obj)->recv_wait_q;
    } else if (task->block_reason == BLOCK_EVENT) {
        head = &((cgrtos_event_group_t *)task->block_obj)->wait_q;
    } else if (task->block_reason == BLOCK_STREAM_SEND) {
        head = &((cgrtos_stream_buffer_t *)task->block_obj)->send_wait_q;
    } else if (task->block_reason == BLOCK_STREAM_RECV) {
        head = &((cgrtos_stream_buffer_t *)task->block_obj)->recv_wait_q;
    } else if (task->block_reason == BLOCK_QUEUE_SET) {
        head = &((cgrtos_queue_set_t *)task->block_obj)->wait_q;
    }
    if (head) {
        cgrtos_wait_list_remove(head, task);
    }
    sched_delayed_remove(task);
}

/**
 * @brief 按 wake_tick 插入全局延迟唤醒链表
 * @details 设置 wake_tick；先 sched_delayed_remove 防双链；list_insert_sorted 升序插入。
 * @param[in] task 任务 TCB
 * @param[in] wake_tick 唤醒时刻（g_ticks 基准）
 * @return 无
 * @retval 无
 * @note block/block_until 内部
 * @warning 无
 * @attention @internal ❌ ISR；❌ block/switch
 */
static void sched_delayed_insert(cgrtos_task_t *task, tick_t wake_tick)
{
    /* 1. 设置 task->wake_tick = wake_tick */
    task->wake_tick = wake_tick;
    /* 2. 先摘旧节点再按 wake_tick 升序插入（防双链成环） */
    sched_delayed_remove(task);
    list_insert_sorted(&g_delayed_list, &task->delayed_item, wake_tick);
}

/**
 * @brief 阻塞当前任务并可选加入延迟唤醒
 * @details READY 则 remove_ready；state=BLOCKED；按 timeout 插入延迟链或 portMAX_DELAY 移除延迟。
 * @param[in] task 任务 TCB
 * @param[in] reason 阻塞原因
 * @param[in] obj 关联 IPC 对象
 * @param[in] timeout 超时 tick；portMAX_DELAY 无超时；0 立即
 * @return 无
 * @retval 无
 * @note idle(id==0) 忽略
 * @warning 调用方通常已持 IPC 锁
 * @attention ❌ ISR；✅ block/switch
 */
void cgrtos_sched_block(cgrtos_task_t *task, block_reason_t reason,
                        void *obj, tick_t timeout)
{
    /* 1. task 为空或 idle（id==0）则返回 */
    if (!task || task->id == 0) {
        return;
    }

    /* 2. 若 task 当前 READY，先从就绪队列移除 */
    if (task->state == TASK_READY) {
        cgrtos_sched_remove_ready(task);
    }

    /* 3. 设置 state=BLOCKED、block_reason、block_obj */
    task->state = TASK_BLOCKED;
    task->block_reason = reason;
    task->block_obj = obj;
    CGRTOS_TRACE(CGRTOS_TRACE_BLOCK, task->id, (uint32_t)reason);

    /* 4. 按 timeout 分支处理延迟链 */
    if (timeout == portMAX_DELAY) {
        /* 4a. portMAX_DELAY：wake_tick=0，从延迟链移除 */
        task->wake_tick = 0;
        sched_delayed_remove(task);
    } else if (timeout == 0) {
        /* 4b. timeout==0：wake_tick=g_ticks，立即插入延迟链 */
        task->wake_tick = g_ticks;
        sched_delayed_insert(task, g_ticks);
    } else {
        /* 4c. 否则：插入 g_ticks+timeout 时刻的延迟链 */
        sched_delayed_insert(task, g_ticks + timeout);
    }
}

/**
 * @brief 阻塞任务直到绝对 wake_tick
 * @details READY 则 remove_ready；BLOCKED；sched_delayed_insert 绝对时刻。
 * @param[in] task 任务 TCB
 * @param[in] reason 阻塞原因
 * @param[in] obj 关联对象
 * @param[in] wake_tick 绝对唤醒 tick
 * @return 无
 * @retval 无
 * @note 调用方应先判断是否已到期
 * @warning idle 忽略
 * @attention ❌ ISR；✅ block/switch
 */
void cgrtos_sched_block_until(cgrtos_task_t *task, block_reason_t reason,
                              void *obj, tick_t wake_tick)
{
    /* 1. task 为空或 idle 则返回 */
    if (!task || task->id == 0) {
        return;
    }

    /* 2. 若 READY 则先从就绪队列移除 */
    if (task->state == TASK_READY) {
        cgrtos_sched_remove_ready(task);
    }

    /* 3. 设置 BLOCKED 状态及 block_reason、block_obj */
    task->state = TASK_BLOCKED;
    task->block_reason = reason;
    task->block_obj = obj;
    /* 4. 按绝对 wake_tick 插入延迟链 */
    sched_delayed_insert(task, wake_tick);
}

/**
 * @brief 唤醒阻塞任务并加入就绪队列
 * @details 非 BLOCKED 返回 pdFAIL；清延迟与 block 字段；add_ready；SMP 跨核则 IPI 目标 hart。
 * @param[in] task 任务 TCB
 * @return pdPASS 成功；pdFAIL 非 BLOCKED
 * @retval pdPASS 已唤醒
 * @retval pdFAIL 状态不对
 * @note 可从中断或任务上下文调用
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_sched_unblock(cgrtos_task_t *task)
{
    /* 1. task 为空或 state != TASK_BLOCKED 则返回 pdFAIL */
    if (!task || task->state != TASK_BLOCKED) {
        return pdFAIL;
    }

    /* 2. 从延迟链移除，清除 block_reason、block_obj、wake_tick */
    sched_delayed_remove(task);
    task->block_reason = BLOCK_NONE;
    task->block_obj = 0;
    task->wake_tick = 0;
    /* 3. 调用 cgrtos_sched_add_ready 加入就绪队列 */
    cgrtos_sched_add_ready(task);
    CGRTOS_TRACE(CGRTOS_TRACE_UNBLOCK, task->id, task->wake_ok);

#if CONFIG_NUM_CORES > 1
    {
        /* 4. SMP：若目标核 != 本核且目标核已上线，向其发 IPI */
        uint8_t dst = cgrtos_sched_target_core(task);
        uint8_t here = (uint8_t)read_csr(mhartid);
        if (dst != here && CGRTOS_CORE_ONLINE(dst)) {
            cgrtos_smp_send_ipi(dst);
        }
    }
#endif
    return pdPASS;
}

/**
 * @brief 挂起调度器（嵌套计数 +1）
 * @details enter_critical；g_sched_suspend_count++；exit_critical。
 * @return 无
 * @retval 无
 * @note 挂起期间 switch_from_trap 不切换
 * @warning 须与 resume 配对
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_sched_suspend(void)
{
    cgrtos_enter_critical();
    g_sched_suspend_count++;
    cgrtos_exit_critical();
}

/**
 * @brief 恢复调度器；嵌套归零时触发 yield
 * @details enter_critical 递减 suspend_count；exit_critical；归零则 cgrtos_sched_yield。
 * @return 无
 * @retval 无
 * @note 与 suspend 配对
 * @warning resume 后可能立即切换
 * @attention ❌ ISR；✅ block/switch
 */
void cgrtos_sched_resume(void)
{
    cgrtos_enter_critical();
    if (g_sched_suspend_count > 0) {
        g_sched_suspend_count--;
    }
    cgrtos_exit_critical();

    if (g_sched_suspend_count == 0) {
        cgrtos_sched_yield();
    }
}

/**
 * @brief 主动让出 CPU（任务上下文 ecall 进入 trap 切换）
 * @details 调度挂起或未运行则返回；置 yield_pending 与 force_yield；ISR 内仅置标志；否则 ecall。
 * @return 无
 * @retval 无
 * @note noinline 保证 ecall 返回路径正确
 * @warning ISR 请用 yield_from_isr
 * @attention ❌ ISR；✅ block/switch
 */
void __attribute__((noinline)) cgrtos_sched_yield(void)
{
    if (g_sched_suspend_count || !g_sched_run) {
        return;
    }

    uint8_t cpu = (uint8_t)read_csr(mhartid);
    g_yield_pending[cpu] = 1;
    g_force_yield[cpu] = 1;

    if (cgrtos_in_isr()) {
        return;
    }

    asm volatile("ecall" ::: "memory");
}

/**
 * @brief 从中断上下文请求调度切换
 * @details 置本核 g_yield_pending=1；trap 返回路径完成 switch_from_trap。
 * @return 无
 * @retval 无
 * @note 不在 ISR 内直接 ecall
 * @warning 无
 * @attention ✅ ISR；✅ block/switch
 */
void cgrtos_sched_yield_from_isr(void)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    g_yield_pending[cpu] = 1;
}

/**
 * @brief 陷阱返回路径上的上下文切换核心逻辑
 * @details 挂起/未运行返回原 sp；pick_next；PRIORITY 粘性判断；换出 requeue；换入更新 g_current 与统计；返回 next->sp。
 * @param[in] sp 当前任务陷阱栈指针
 * @return 下一任务应恢复的 sp；未切换时原 sp
 * @retval next->sp 已切换
 * @retval sp 粘性或未切换
 * @note trap_vector 调用；持 ready_lock
 * @warning DELETED 任务在切换后 reclaim
 * @attention ✅ ISR；✅ block/switch
 */
uint64_t *cgrtos_sched_switch_from_trap(uint64_t *sp)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *cur = g_current[cpu];
    cgrtos_task_t *next;

    /* 1. 若调度挂起或未运行，返回原 sp */
    if (g_sched_suspend_count || !g_sched_run) {
        return sp;
    }

    /* 2. 保存当前任务 sp 到 cur->sp */
    if (cur) {
        cur->sp = sp;
    }

    /* 3. 关 IRQ 并持 g_ready_lock，调用 sched_pick_next 选取 next */
    uint64_t flags = cgrtos_irq_save();
    cgrtos_spin_lock(&g_ready_lock);

    next = sched_pick_next(cpu);

    /* 4. 粘性调度判断（PRIORITY/Hybrid RT） */
    if (cur && cur->id > 0 && cur->state == TASK_RUNNING) {
        int stick = 0;
        int forced = g_force_yield[cpu];
        g_force_yield[cpu] = 0;

        if (!forced &&
            (cur->policy == SCHED_PRIORITY ||
             (cur->policy == SCHED_HYBRID &&
              cur->prio >= CONFIG_RT_PRIO_THRESHOLD))) {
#if CONFIG_USE_PREEMPT_THRESH
            uint8_t pt = cur->preempt_thresh;
#else
            uint8_t pt = cur->prio;
#endif
            if (!next || next == &g_idle[cpu]) {
                stick = 1;
            } else if (sched_uses_cfs(next) && !sched_uses_edf(next)) {
                /* CFS 不能抢占纯 RT 优先级任务 */
                stick = 1;
            } else if (sched_uses_priority(next) && next->prio <= pt &&
                       !(sched_uses_edf(next) &&
                         next->deadline <= g_ticks + CONFIG_MCEDF_PRIO_SLACK_TICKS)) {
                /* 抢占阈值：仅 next->prio > pt 才真正抢占 */
                stick = 1;
            } else if (sched_uses_edf(next) &&
                       next->deadline > g_ticks + CONFIG_MCEDF_PRIO_SLACK_TICKS) {
                stick = 1;
            }
        }

        if (stick) {
            cgrtos_spin_unlock(&g_ready_lock);
            cgrtos_irq_restore(flags);
            return sp;
        }

        /* 5. 当前任务换出，重新入就绪队列 */
        cur->state = TASK_READY;
        sched_add_ready_locked(cur);
    } else if (cur && cur->state == TASK_DELETED) {
        /* 稍后在 g_current 更新后再 reclaim */
    }

    /* 6. 确定 next 并出队 */
    if (!next || !next->sp) {
        next = &g_idle[cpu];
    } else if (next->id > 0 && next->state == TASK_READY) {
        sched_remove_ready_locked(next);
    }

    /* 7. 执行或跳过切换 */
    if (cur != next) {
        uint64_t *next_sp;
        task_id_t from_id = cur ? cur->id : 0;
        task_id_t to_id = next->id;

        g_current[cpu] = next;
        next->state = TASK_RUNNING;
        next->run_cpu = cpu;
        next->last_run = g_ticks;
#if CONFIG_SCHED_STATS
        if (next->id > 0) {
            tick_t lat = g_ticks - next->ready_since;
            next->last_sched_latency = lat;
            next->sched_latency_sum += lat;
            next->sched_latency_samples++;
            if (lat > next->max_sched_latency) {
                next->max_sched_latency = lat;
            }
            if (lat > g_sched_max_latency) {
                g_sched_max_latency = lat;
            }
            g_sched_latency_samples++;
        }
#endif
        g_cs_count++;
        g_cs_count_core[cpu]++;
        next_sp = next->sp;

        /* 已离开 DELETED 任务：现在可安全清 id */
        if (cur && cur->state == TASK_DELETED) {
            cgrtos_task_reclaim_deleted(cur);
        }

        /* 缩短 ready_lock：先解锁再做栈检查 / Trace（减切换路径持锁时间） */
        cgrtos_spin_unlock(&g_ready_lock);
        cgrtos_irq_restore(flags);

        CGRTOS_TRACE(CGRTOS_TRACE_SWITCH, from_id, to_id);

#if CONFIG_CHECK_STACK_OVERFLOW
        /* 切换切入前检查下一任务金丝雀（不持 ready_lock） */
        if (next->id > 0 && next->stack[0] != STACK_CANARY_VALUE) {
            cgrtos_task_handle_stack_overflow(next);
        }
#endif
        return next_sp;
    }

    /* 8. 相同任务：恢复 cur 为 RUNNING，解锁，返回原 sp */
    if (cur) {
        cur->state = TASK_RUNNING;
    }
    cgrtos_spin_unlock(&g_ready_lock);
    cgrtos_irq_restore(flags);
    return sp;
}

/**
 * @brief 处理到期延迟唤醒（超时 unblock）
 * @details enter_critical；循环 peek 延迟链队头，wake_tick<=g_ticks 则 purge+unblock(wake_ok=0)；exit_critical。
 * @return 无
 * @retval 无
 * @note hart0 tick 调用；与 IPC 串行化
 * @warning 持 g_klock
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_process_delayed_tasks(void)
{
    /* 1. 进入临界区（与 IPC block/unblock 串行化） */
    cgrtos_enter_critical();
    while (1) {
        list_item_t *item = list_peek_head(&g_delayed_list);
        /* 2a. 队空或 wake_tick > g_ticks 则退出 */
        if (!item || item->value > g_ticks) {
            break;
        }

        /* 2b. 移除队头 delayed_item，反算 task 指针 */
        list_remove(&g_delayed_list, item);
        cgrtos_task_t *task = (cgrtos_task_t *)((uint8_t *)item -
                                                offsetof(cgrtos_task_t, delayed_item));

        /* 2c. 若 task 非 BLOCKED 则跳过 */
        if (task->state != TASK_BLOCKED) {
            continue;
        }

        /* 2d. 置 wake_ok=0（超时），purge 等待队列，unblock 唤醒 */
        task->wake_ok = 0; /* 超时唤醒 */
        cgrtos_task_purge_waits(task);
        cgrtos_sched_unblock(task);
    }
    /* 3. 退出临界区 */
    cgrtos_exit_critical();
}

/**
 * @brief 本核时间片记账与抢占请求（不含全局 g_ticks）
 * @details exec++/CFS vruntime/slice--；片尽则 rotate 并 need_yield；每次 tick 评估就绪集；ISR 置 pending 否则 yield。
 * @return 无
 * @retval 无
 * @note hart0 定时器或次核 g_remote_tick+IPI 调用
 * @warning 无
 * @attention ✅ ISR；✅ block/switch
 */
void cgrtos_tick_local(void)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *cur = g_current[cpu];

    if (cur && cur->id > 0) {
        /* 2. 运行统计 + 可选栈金丝雀抽检 */
        cur->exec++;
#if CONFIG_CHECK_STACK_OVERFLOW && CONFIG_STACK_CHECK_ON_TICK
        if (cur->stack[0] != STACK_CANARY_VALUE) {
            cgrtos_task_handle_stack_overflow(cur);
        }
#endif
        if (sched_uses_cfs(cur)) {
            cur->vruntime += 1000000ULL / ((uint64_t)cur->prio + 1ULL);
        }
        if (cur->slice_remain > 0) {
            cur->slice_remain--;
        }
    }

    int need_yield = 0;
    if (cur && cur->id > 0 && cur->slice_remain == 0 &&
        cur->state == TASK_RUNNING) {
        if (cur->policy == SCHED_RR || sched_uses_cfs(cur) ||
            cur->policy == SCHED_EDF) {
            sched_rotate_time_slice(cur);
            need_yield = 1;
        } else {
            cur->slice_remain = CONFIG_TIME_SLICE_TICKS;
        }
    }

    /* 每次 tick 重新评估就绪集，允许高优先级抢占 */
    if (g_sched_suspend_count == 0) {
        need_yield = 1;
    }

    if (need_yield) {
        if (cgrtos_in_isr()) {
            g_yield_pending[cpu] = 1;
        } else {
            cgrtos_sched_yield();
        }
    }
}

/**
 * @brief 系统 tick 中断处理（每核部分 + hart0 全局工作）
 * @details hart0：g_ticks++、延迟/EDF/软定时器/hook/LB/remote_tick IPI；全核 cgrtos_tick_local。
 * @return 无
 * @retval 无
 * @note CLINT 定时器 ISR 入口
 * @warning 无
 * @attention ✅ ISR；✅ block/switch
 */
void cgrtos_tick_handler(void)
{
    /* 1. 读取本核 cpu 编号 */
    uint8_t cpu = (uint8_t)read_csr(mhartid);

    if (cpu == 0) {
        /* 2a. 原子递增 g_ticks */
        __atomic_fetch_add(&g_ticks, 1, __ATOMIC_SEQ_CST);
        /* 2b. 处理延迟超时唤醒 */
        sched_process_delayed_tasks();
#if CONFIG_USE_EDF
        /* 2c. 处理 EDF 释放轮 */
        sched_process_edf_releases();
#endif
        /* 2d. 软件定时器 tick */
        cgrtos_timer_process_tick();
#if CONFIG_USE_HOOKS
        /* 2e. 可选 tick hook */
        if (g_tick_hook) {
            g_tick_hook();
        }
#endif
        g_lb_tick++;
        /* 2f. 周期性负载均衡 */
        if ((g_lb_tick % CONFIG_LOAD_BALANCE_PERIOD) == 0) {
            cgrtos_sched_load_balance();
        }
#if CONFIG_SMP_ENABLE && CONFIG_NUM_CORES > 1
        /* 2g. SMP：向已在线次核置 g_remote_tick 并发 IPI */
        if (g_secondary_online) {
            for (uint8_t c = 1; c < CONFIG_NUM_CORES; c++) {
                if (CGRTOS_CORE_ONLINE(c)) {
                    g_remote_tick[c] = 1;
                    cgrtos_smp_send_ipi(c);
                }
            }
        }
#endif
    }

    /* 3. 所有核调用 cgrtos_tick_local 做本地时间片记账 */
    cgrtos_tick_local();
}

/**
 * @brief 统计指定核上就绪任务总数
 * @details 累加各优先级 count + CFS count + MC-EDF 全局上可运行于本核的近似计数。
 * @param[in] cpu 核编号
 * @return 就绪任务数
 * @retval >=0 计数
 * @retval 0 cpu 越界
 * @note EDF 全局计数为无锁近似
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
uint32_t cgrtos_sched_ready_count(uint8_t cpu)
{
    if (cpu >= CONFIG_NUM_CORES) {
        return 0;
    }
    uint32_t n = 0;
    for (int p = 0; p <= CONFIG_MAX_PRIORITY; p++) {
        n += g_ready[cpu][p].count;
    }
    n += list_count(&g_cfs_ready[cpu]);
    /* MC-EDF：全局链/堆上存在可在本核运行的任务则计 1（无锁近似，供 steal/统计） */
    {
#if CONFIG_USE_EDF && CONFIG_USE_EDF_HEAP
        uint32_t k;
        for (k = 0; k < g_edf_heap_n; k++) {
            if (sched_edf_affinity_ok(g_edf_heap[k], cpu)) {
                n += 1;
                break;
            }
        }
#else
        list_item_t *it = g_edf_global.head;
        while (it) {
            cgrtos_task_t *t = (cgrtos_task_t *)((uint8_t *)it -
                                                 offsetof(cgrtos_task_t, edf_item));
            if (sched_edf_affinity_ok(t, cpu)) {
                n += 1;
                break;
            }
            it = it->next;
        }
#endif
    }
    return n;
}

/**
 * @brief 计算任务在负载均衡中的权重
 * @details idle/EDF 返回 0；CFS 返回 LB_CFS_WEIGHT；RT 返回 (prio+1)*LB_PRIO_SCALE。
 * @param[in] task 任务 TCB
 * @return 权重值
 * @retval >0 可计负载
 * @retval 0 不计入
 * @note 权重越高越不宜迁出
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static uint32_t sched_task_weight(cgrtos_task_t *task)
{
    if (!task || task->id == 0) {
        return 0;
    }
    if (task->policy == SCHED_EDF) {
        return 0;
    }
    if (sched_uses_cfs(task)) {
        return CONFIG_LB_CFS_WEIGHT;
    }
    return (uint32_t)(task->prio + 1U) * CONFIG_LB_PRIO_SCALE;
}

/**
 * @brief 判断 READY 任务是否可迁移到其它核
 * @details 非 READY/idle/硬亲和/EDF/高 prio 定时器守护返回 0；否则 1。
 * @param[in] task 任务 TCB
 * @return 非零可迁移
 * @retval 1 可迁移
 * @retval 0 不可
 * @note LB 与 idle steal 筛选
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static int sched_task_migratable(cgrtos_task_t *task)
{
    if (!task || task->id == 0 || task->state != TASK_READY) {
        return 0;
    }
    if (task->cpu_aff != 0xFF) {
        return 0;
    }
    if (task->policy == SCHED_EDF) {
        return 0;
    }
    if (task->prio >= CONFIG_TIMER_TASK_PRIO - 1) {
        return 0;
    }
    return 1;
}

/**
 * @brief 计算指定核的加权负载
 * @details 遍历 g_tasks：READY 且 target_core==cpu 累加 weight；当前 RUNNING 非 idle 也累加。
 * @param[in] cpu 核编号
 * @return 加权负载和
 * @retval >=0 负载值
 * @retval 0 cpu 越界
 * @note LB 决策依据
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
uint32_t cgrtos_sched_core_load(uint8_t cpu)
{
    if (cpu >= CONFIG_NUM_CORES) {
        return 0;
    }
    uint32_t load = 0;
    for (uint32_t i = 0; i < CONFIG_MAX_TASKS; i++) {
        cgrtos_task_t *t = &g_tasks[i];
        if (t->id == 0) {
            continue;
        }
        if (t->state == TASK_READY && cgrtos_sched_target_core(t) == cpu) {
            load += sched_task_weight(t);
        }
    }
    cgrtos_task_t *cur = g_current[cpu];
    if (cur && cur->id > 0 && cur->state == TASK_RUNNING) {
        load += sched_task_weight(cur);
    }
    return load;
}

/**
 * @brief 返回负载最低的可运行核（SMP）
 * @details 单核或未上线 secondary 返回 0；比较 l0/l1 返回较低者。
 * @return 核编号
 * @retval 0 或 1 较低负载核
 * @retval 0 单核或未就绪
 * @note 任务创建默认亲和参考
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
uint8_t cgrtos_sched_least_loaded_core(void)
{
#if CONFIG_NUM_CORES < 2
    return 0;
#else
    if (!g_secondary_online) {
        return 0;
    }
    uint32_t l0 = cgrtos_sched_core_load(0);
    uint32_t l1 = cgrtos_sched_core_load(1);
    return (l1 < l0) ? 1 : 0;
#endif
}

/**
 * @brief 将 READY 任务迁移到 dst 并发送 IPI
 * @details remove_ready；run_cpu=dst；add_ready；IPI dst；g_lb_migrate_count++。
 * @param[in] task 任务 TCB
 * @param[in] dst 目标核
 * @return 无
 * @retval 无
 * @note LB push 与 idle steal 共用
 * @warning 须 migratable
 * @attention @internal ✅ ISR；❌ block/switch
 */
static void sched_migrate_task(cgrtos_task_t *task, uint8_t dst)
{
    /* 1. 从就绪队列 remove_ready */
    cgrtos_sched_remove_ready(task);
    /* 2. 更新 task->run_cpu = dst */
    task->run_cpu = dst;
    /* 3. add_ready 到目标核队列 */
    cgrtos_sched_add_ready(task);
    /* 4. 向 dst 发 IPI，递增 g_lb_migrate_count */
    cgrtos_smp_send_ipi(dst);
    g_lb_migrate_count++;
}

/**
 * @brief 在 busy 核上选取最佳迁移牺牲者
 * @details 遍历 migratable 且 target_core==src；选 weight 最小，同 weight 选 vruntime 大。
 * @param[in] src 源核编号
 * @return 任务指针；无可迁移 NULL
 * @retval 非 NULL victim
 * @retval NULL 无候选
 * @note push LB 与 pull steal 共用
 * @warning 无
 * @attention @internal ✅ ISR；❌ block/switch
 */
static cgrtos_task_t *sched_pick_victim(uint8_t src)
{
    cgrtos_task_t *best = 0;
    uint32_t best_w = 0xFFFFFFFFu;
    tick_t best_vr = 0;

    for (uint32_t i = 0; i < CONFIG_MAX_TASKS; i++) {
        cgrtos_task_t *t = &g_tasks[i];
        if (!sched_task_migratable(t)) {
            continue;
        }
        if (cgrtos_sched_target_core(t) != src) {
            continue;
        }
        uint32_t w = sched_task_weight(t);
        if (w == 0) {
            continue;
        }
        if (w < best_w || (w == best_w && t->vruntime >= best_vr)) {
            best = t;
            best_w = w;
            best_vr = t->vruntime;
        }
    }
    return best;
}

/**
 * @brief 加权 push 负载均衡（hart0 周期性调用）
 * @details 未运行/挂起/单核/次核离线则返回；算 diff>HYST 时在 busy 核 pick_victim 迁移到 idle 核，最多 4 次。
 * @return 无
 * @retval 无
 * @note tick 周期 CONFIG_LOAD_BALANCE_PERIOD
 * @warning 仅 hart0 调用
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_sched_load_balance(void)
{
    /* 1. 调度未运行或挂起则返回 */
    if (!g_sched_run || g_sched_suspend_count) {
        return;
    }
#if CONFIG_NUM_CORES < 2
    return;
#else
    /* 1. 单核或次核未上线则返回 */
    if (!g_secondary_online) {
        return;
    }

    /* 2. 计算 l0、l1，确定 busy 核与 idle 核，计算负载差 diff */
    uint32_t l0 = cgrtos_sched_core_load(0);
    uint32_t l1 = cgrtos_sched_core_load(1);
    uint8_t busy = (l0 >= l1) ? 0 : 1;
    uint8_t idle = (uint8_t)(1U - busy);
    int32_t diff = (int32_t)cgrtos_sched_core_load(busy) -
                   (int32_t)cgrtos_sched_core_load(idle);

    /* 3. 当 diff > HYST 且 moved < 4，循环迁移 victim */
    int moved = 0;
    while (diff > (int32_t)CONFIG_LOAD_BALANCE_HYST && moved < 4) {
        /* 3a. 在 busy 核选 victim */
        cgrtos_task_t *victim = sched_pick_victim(busy);
        /* 3b. 无 victim 则退出 */
        if (!victim) {
            break;
        }
        /* 3c. sched_migrate_task 迁移到 idle 核 */
        sched_migrate_task(victim, idle);
        moved++;
        /* 3d. 重新计算 diff，继续循环 */
        diff = (int32_t)cgrtos_sched_core_load(busy) -
               (int32_t)cgrtos_sched_core_load(idle);
    }
#endif
}

/**
 * @brief Idle 任务中的工作窃取（从 peer 核 pull 任务）
 * @details 未启用 STEAL/单核/未运行/挂起/本核有就绪/对端不足则返回；pick_victim 迁移并 steal++。
 * @return 无
 * @retval 无
 * @note CONFIG_SMP_IDLE_STEAL 控制
 * @warning 本地有工作时不窃取
 * @attention ❌ ISR；✅ block/switch
 */
void cgrtos_sched_idle_steal(void)
{
#if !CONFIG_SMP_IDLE_STEAL
    /* 1. CONFIG_SMP_IDLE_STEAL 未启用则返回 */
    return;
#elif CONFIG_NUM_CORES < 2
    /* 1. 单核则返回 */
    return;
#else
    /* 2. 调度未运行、次核未上线或挂起则返回 */
    if (!g_sched_run || !g_secondary_online || g_sched_suspend_count) {
        return;
    }

    uint8_t cpu = (uint8_t)read_csr(mhartid);
    uint8_t other = (cpu == 0) ? 1 : 0;

    /* 3. 本核已有就绪任务则返回（不窃取） */
    if (cgrtos_sched_ready_count(cpu) > 0) {
        return;
    }
    /* 4. 对端就绪数 < CONFIG_LB_STEAL_MIN 则返回 */
    if (cgrtos_sched_ready_count(other) < CONFIG_LB_STEAL_MIN) {
        return;
    }

    /* 5. 从对端选 victim，迁移到本核，递增 steal 计数，置 yield_pending */
    cgrtos_task_t *victim = sched_pick_victim(other);
    if (victim) {
        sched_migrate_task(victim, cpu);
        g_lb_steal_count++;
        g_yield_pending[cpu] = 1;
    }
#endif
}

/**
 * @brief 初始化调度器内部数据结构
 * @details 清零 ready/bitmap；init CFS/EDF/释放轮/延迟链；suspend=0；ready_lock=0；force_yield 清零。
 * @return 无
 * @retval 无
 * @note cgrtos_init 早期调用
 * @warning 调度未启动前调用
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_sched_init(void)
{
    memset(g_ready, 0, sizeof(g_ready));
    memset(g_ready_bitmap, 0, sizeof(g_ready_bitmap));
    for (int i = 0; i < CONFIG_NUM_CORES; i++) {
        list_init(&g_cfs_ready[i]);
    }
    list_init(&g_edf_global);
#if CONFIG_USE_EDF && CONFIG_USE_EDF_HEAP
    g_edf_heap_n = 0;
    memset(g_edf_heap, 0, sizeof(g_edf_heap));
#endif
    for (int i = 0; i < CONFIG_EDF_RELEASE_SLOTS; i++) {
        list_init(&g_edf_rel[i]);
    }
    list_init(&g_delayed_list);
    g_edf_rel_cursor = 0;
    g_sched_suspend_count = 0;
    g_ready_lock = 0;
    g_lb_tick = 0;
    memset((void *)g_force_yield, 0, sizeof(g_force_yield));
}

#if CONFIG_SCHED_STATS
/**
 * @brief 读取调度延迟统计快照
 * @details 关 IRQ 持 ready_lock；复制 g_sched_max_latency 与 g_sched_latency_samples 到输出指针。
 * @param[out] max_latency_global 最大调度延迟；可 NULL
 * @param[out] samples 采样计数；可 NULL
 * @return 无
 * @retval 无
 * @note CONFIG_SCHED_STATS 编译
 * @warning 指针可为 NULL 跳过对应字段
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_sched_stats_get(tick_t *max_latency_global, uint32_t *samples)
{
    uint64_t flags = cgrtos_irq_save();
    cgrtos_spin_lock(&g_ready_lock);
    if (max_latency_global) {
        *max_latency_global = g_sched_max_latency;
    }
    if (samples) {
        *samples = g_sched_latency_samples;
    }
    cgrtos_spin_unlock(&g_ready_lock);
    cgrtos_irq_restore(flags);
}

/**
 * @brief 重置调度延迟统计
 * @details 关 IRQ 持 ready_lock；清零全局与各任务 per-task 延迟统计字段。
 * @return 无
 * @retval 无
 * @note CONFIG_SCHED_STATS 编译
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_sched_stats_reset(void)
{
    uint64_t flags = cgrtos_irq_save();
    cgrtos_spin_lock(&g_ready_lock);
    g_sched_max_latency = 0;
    g_sched_latency_samples = 0;
    for (uint32_t i = 0; i < CONFIG_MAX_TASKS; i++) {
        g_tasks[i].max_sched_latency = 0;
        g_tasks[i].last_sched_latency = 0;
        g_tasks[i].sched_latency_sum = 0;
        g_tasks[i].sched_latency_samples = 0;
    }
    cgrtos_spin_unlock(&g_ready_lock);
    cgrtos_irq_restore(flags);
}
#endif
