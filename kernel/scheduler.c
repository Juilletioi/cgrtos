/**
 * @file scheduler.c
 * @brief CG-RTOS 多策略抢占式 SMP 调度器核心。
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
 * @brief 从就绪位图取最高优先级数值。
 *
 * @param bitmap 优先级位图（bit i 表示优先级 i 有就绪任务）。
 * @return 最高优先级 [0..31]；位图为 0 时返回 -1。
 *
 * @details
 * 1. 若 bitmap 为 0，无就绪任务，返回 -1。
 * 2. 使用 __builtin_clz 计算最高位位置，31 - clz 即为最高优先级数值。
 */
static inline int sched_highest_ready_prio(uint32_t bitmap)
{
    if (!bitmap) {
        return -1;
    }
    return (int)(31 - __builtin_clz(bitmap));
}

/**
 * @brief 判断任务是否走优先级/RR 就绪队列。
 *
 * @param task 任务指针。
 * @return 非零表示使用优先级队列；0 表示不使用。
 *
 * @details
 * 1. task 为 NULL 时返回 0。
 * 2. SCHED_PRIORITY 或 SCHED_RR 策略直接返回 1。
 * 3. SCHED_HYBRID 策略：prio >= CONFIG_RT_PRIO_THRESHOLD 时走 RT 优先级队列。
 * 4. 其余策略返回 0。
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
 * @brief 判断任务是否走 CFS 就绪队列。
 *
 * @param task 任务指针。
 * @return 非零表示使用 CFS 队列；0 表示不使用。
 *
 * @details
 * 1. task 为 NULL 时返回 0。
 * 2. SCHED_CFS 策略直接返回 1。
 * 3. SCHED_HYBRID 策略：prio < CONFIG_RT_PRIO_THRESHOLD 时走 CFS 公平队列。
 * 4. 其余策略返回 0。
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
 * @brief 判断任务是否走 EDF 就绪队列。
 *
 * @param task 任务指针。
 * @return 非零表示 EDF 策略；0 表示非 EDF。
 *
 * @details
 * 1. task 非空且 policy == SCHED_EDF 时返回 1。
 * 2. 否则返回 0。
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
 * @brief EDF 任务是否允许在 cpu 上运行（硬亲和）
 * @param task 任务；@param cpu 核号
 * @return 非 0 表示可在该核运行
 * @details 步骤：1. cpu_aff==0xFF 任意核；2. 否则必须 cpu_aff==cpu。
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
 *
 * @details 步骤：
 * 1. 对本核置 g_yield_pending。
 * 2. 对其余在线核置 yield_pending 并发送 MSIP IPI。
 * @note 在 EDF 释放/入队后调用，使更早 deadline 能立即抢占其它核。
 * @warning 调用方不得持有 g_ready_lock（IPI 路径可能间接争用）。
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
 * @brief 请求 MC-EDF 重评估：临界区内仅置 pending，出临界区再 kick
 * @risk 持 g_klock 时同步 IPI 可能导致对端 enter_critical 自旋死锁。
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
 * @brief 刷新推迟的 MC-EDF kick（由 exit_critical 最外层调用）
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
 * @brief 从全局 MC-EDF 就绪链移除任务（调用方已持锁）
 * @param task EDF 任务
 * @details 步骤：1. 若节点在链上则 list_remove；2. 否则无操作。
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
 * @brief 从 EDF 释放时间轮移除任务。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. task 为空或未在轮上（edf_on_wheel==0）则直接返回。
 * 2. 从 edf_rel_item.value 低 32 位计算所在槽位 slot。
 * 3. 若节点确实在链表中，调用 list_remove 移除。
 * 4. 清除 task->edf_on_wheel 标志。
 *
 * @note 调用方应已持有 g_ready_lock。
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
 * @brief 将 EDF 任务挂入释放时间轮（按 deadline 触发释放）。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. 关 IRQ 并获取 g_ready_lock。
 * 2. 先从时间轮移除 task 的旧条目（若有）。
 * 3. 若非 EDF 或 period==0，解锁返回（无需挂轮）。
 * 4. 计算释放时刻 when = max(deadline, g_ticks)。
 * 5. 计算 delta = when - g_ticks，slot = (cursor + delta) & mask，rounds = delta / slots。
 * 6. 将 rounds 存入 value 高 32 位、slot 存入低 32 位，插入对应槽位链表。
 * 7. 置 edf_on_wheel=1，解锁并恢复 IRQ。
 *
 * @note 非 EDF 或 period==0 时仅移除轮上条目；内部获取 ready_lock。
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
 * @brief EDF 周期释放：推进 deadline 并唤醒/入就绪队列。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. 校验 task 为 EDF 且 period != 0，否则返回。
 * 2. 将 deadline 推进到 g_ticks + period（新周期）。
 * 3. 按当前 state 分支处理：
 *    a. BLOCKED + BLOCK_DELAY：调用 unblock 唤醒；
 *    b. READY：先从就绪队列移除再重新插入（按新 deadline 排序）；
 *    c. 非 RUNNING/SUSPENDED/DELETED：直接 add_ready；
 *    d. RUNNING 等状态不做额外操作。
 * 4. 调用 cgrtos_sched_edf_arm 重新挂入释放轮。
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
 * @brief 处理当前 EDF 释放轮槽位到期任务。
 *
 * @details
 * 1. 关 IRQ 并获取 g_ready_lock。
 * 2. 游标 g_edf_rel_cursor 前进一步（模 slots）。
 * 3. 遍历当前槽位链表：
 *    a. 若 value 高 32 位 rounds > 0，递减 rounds 并跳过（尚未到期）；
 *    b. 若 rounds == 0，从链表移除，清除 edf_on_wheel，收集到 to_release[]。
 * 4. 解锁并恢复 IRQ。
 * 5. 在锁外逐个调用 sched_edf_release 执行实际释放（避免持锁过久）。
 *
 * @note 由 hart0 tick 调用；多轮 delta 通过 value 高 32 位 rounds 递减。
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
 * @brief 计算任务应运行的目标 CPU。
 *
 * @param task 任务指针。
 * @return 目标 hart 编号；task 为 NULL 时返回 0。
 *
 * @details
 * 1. task 为 NULL 时返回 0。
 * 2. 若 cpu_aff != 0xFF（硬亲和）：
 *    a. 越界或次核未上线时回退到 0；
 *    b. 否则返回 cpu_aff。
 * 3. 若 run_cpu < CONFIG_NUM_CORES：
 *    a. run_cpu != 0 且次核未上线时回退到 0；
 *    b. 否则返回 run_cpu。
 * 4. 默认返回 0。
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
 * @brief 将任务加入指定核的优先级就绪队列。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. 根据 task 计算目标核 core 与优先级 prio。
 * 2. 将 task 追加到 g_ready[core][prio] 双向链表尾部。
 * 3. 置 g_ready_bitmap[core] 对应 prio 位。
 *
 * @note 调用方应已持有 g_ready_lock。
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
 * @brief 将任务按 vruntime 插入 CFS 就绪队列。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. 计算任务目标核 core。
 * 2. 调用 list_insert_sorted_asc 按 vruntime 升序插入 g_cfs_ready[core]。
 */
static void sched_add_cfs_ready(cgrtos_task_t *task)
{
    /* 1. 计算任务目标核 core */
    uint8_t core = cgrtos_sched_target_core(task);
    /* 2. 按 vruntime 升序插入 g_cfs_ready[core] */
    list_insert_sorted_asc(&g_cfs_ready[core], &task->cfs_item, task->vruntime);
}

/**
 * @brief 将任务按 deadline 插入全局 MC-EDF 就绪队列（见上方 sched_add_edf_ready）。
 */
static void sched_remove_priority_ready(cgrtos_task_t *task);

/**
 * @brief 将任务加入对应策略的就绪队列（调用方已持 g_ready_lock）。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. task 为空或已 DELETED 则返回。
 * 2. 按策略分支：EDF → sched_add_edf_ready；CFS → sched_add_cfs_ready；否则 → sched_add_priority_ready。
 * 3. 若 slice_remain 为 0，按策略初始化时间片（CFS 用 CONFIG_CFS_SLICE_TICKS，其余用 CONFIG_TIME_SLICE_TICKS）。
 * 4. 将 task->state 置为 TASK_READY。
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
 * @brief 从就绪队列移除任务（调用方已持 g_ready_lock）。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. task 为空或 state != TASK_READY 则返回。
 * 2. 按策略从对应队列移除：EDF/CFS 链表或优先级双向链表。
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
 * @brief 将任务加入对应策略的就绪队列。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. task 为空或已 DELETED 则返回。
 * 2. 关 IRQ 并获取 g_ready_lock（防止 timer/IPI 重入死锁）。
 * 3. 调用 sched_add_ready_locked 执行实际入队。
 * 4. 解锁并恢复 IRQ。
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
 * @brief 从优先级就绪队列移除任务。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. 定位 task 所在核 core 与优先级 prio 的 ready_list。
 * 2. 更新双向链表 prev/next 指针，维护 head/tail。
 * 3. 清空 task 的 next/prev，count 递减。
 * 4. 若该优先级队列变空，清除 g_ready_bitmap 对应位。
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
 * @brief 从就绪队列移除任务（按策略选择队列）。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. task 为空或 state != TASK_READY 则返回。
 * 2. 关 IRQ 并获取 g_ready_lock。
 * 3. 调用 sched_remove_ready_locked 执行实际出队。
 * 4. 解锁并恢复 IRQ。
 *
 * @note 仅 TASK_READY 状态有效。
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
 * @brief 在指定核上选取最高优先级且亲和匹配的就绪任务。
 *
 * @param cpu 核编号。
 * @return 任务指针；无可用任务时返回 NULL。
 *
 * @details
 * 1. 从 g_ready_bitmap[cpu] 取最高优先级 prio。
 * 2. 遍历 g_ready[cpu][prio] 链表，跳过 cpu_aff 不匹配的任务。
 * 3. 返回第一个亲和匹配的任务；无匹配则返回 NULL。
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
 * @brief 在指定核上选取 vruntime 最小的 CFS 任务。
 *
 * @param cpu 核编号。
 * @return 任务指针；队列为空时返回 NULL。
 *
 * @details
 * 1. 查看 g_cfs_ready[cpu] 链表头（vruntime 最小）。
 * 2. 通过 offsetof 从 cfs_item 反算 task 指针并返回。
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
 * @brief MC-EDF（Global EDF）：为本核分配应运行的 EDF 任务
 *
 * @param cpu 核编号
 * @return 分配给本核的 EDF 任务；无则 NULL
 *
 * @details
 * 全局规则：在线核数为 m 时，系统中 deadline 最早的至多 m 个就绪 EDF
 * 任务应占用 m 个核（经典 Global / MC-EDF）。
 *
 * 分配步骤（在 g_ready_lock 下，确定性、无额外迁移表）：
 * 1. 枚举在线核列表 cores[0..n)。
 * 2. 按 deadline 升序遍历 g_edf_global。
 * 3. 硬亲和（cpu_aff!=0xFF）：若目标核空闲且在线，则占该核一席。
 * 4. 软亲和：优先占 run_cpu（若空闲在线），否则占第一个空闲在线核。
 * 5. 已填满 n 席则停止。
 * 6. 返回 assigned[cpu]。
 *
 * @note RUNNING 任务不在就绪链中；换出时先 pick 再 requeue，由锁串行化多核。
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
 * @brief 综合 MC-EDF / 优先级 / CFS 选取下一运行任务
 *
 * @param cpu 核编号
 * @return 下一任务；均无则返回该核 idle
 *
 * @details 步骤：
 * 1. 取 MC-EDF 分配、优先级队头、CFS 队头。
 * 2. 若有 EDF：无 RT 优先级候选，或 deadline 在
 *    CONFIG_MCEDF_PRIO_SLACK_TICKS 窗口内 → 选 EDF（硬实时带）。
 * 3. 否则选优先级任务。
 * 4. 否则选 CFS。
 * 5. 否则 idle。
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
 * @brief 时间片耗尽时刷新 slice_remain（不重入就绪队列）。
 *
 * @param task 当前运行任务。
 *
 * @details
 * 1. task 为空或 idle（id==0）则返回。
 * 2. 按策略重置 slice_remain：CFS 用 CONFIG_CFS_SLICE_TICKS，其余用 CONFIG_TIME_SLICE_TICKS。
 * 3. 不修改就绪队列——当前任务仍为 RUNNING，实际 requeue 在 switch_from_trap 完成。
 *
 * @note 当前任务仍为 RUNNING；实际 requeue 在 switch_from_trap 完成。
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
 * @brief 将任务按优先级插入 IPC 等待队列（兼容封装）。
 *
 * @param head 等待队列头指针。
 * @param task 待插入任务。
 *
 * @details
 * 1. 委托 cgrtos_wait_list_add_priority 按优先级降序插入。
 */
void cgrtos_wait_list_add(cgrtos_task_t *volatile *head, cgrtos_task_t *task)
{
    cgrtos_wait_list_add_priority(head, task);
}

/**
 * @brief 将任务按优先级降序插入等待队列。
 *
 * @param head 等待队列头指针。
 * @param task 待插入任务。
 *
 * @details
 * 1. 从头遍历等待队列，跳过 prio >= task->prio 的节点。
 * 2. 在 cur 前插入 task，维护双向链表 prev/next。
 * 3. 若插入队头，更新 *head。
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
 * @brief 从等待队列移除指定任务。
 *
 * @param head 等待队列头指针。
 * @param task 待移除任务。
 *
 * @details
 * 1. head 或 task 为空则返回。
 * 2. 更新 prev/next 指针；若 task 是队头则更新 *head。
 * 3. 清空 task 的 next/prev。
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
 * @brief 弹出等待队列中优先级最高的任务。
 *
 * @param head 等待队列头指针。
 * @return 队头任务；队列为空时返回 NULL。
 *
 * @details
 * 1. 取 *head 作为最高优先级任务（队列按 prio 降序维护）。
 * 2. 调用 cgrtos_wait_list_remove 将其从队列移除。
 * 3. 返回该任务指针。
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
 * @brief 从延迟链表中移除任务。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. 若 task 的 delayed_item 在 g_delayed_list 中，调用 list_remove 移除。
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
 * @brief 阻塞任务从 IPC/事件等待队列及延迟链表中清除。
 *
 * @param task 任务指针。
 *
 * @details
 * 1. task 为空或 state != TASK_BLOCKED 则返回。
 * 2. 根据 block_reason 定位对应 IPC 对象的 wait_q 头指针。
 * 3. 若找到 wait_q，调用 cgrtos_wait_list_remove 从等待队列移除。
 * 4. 调用 sched_delayed_remove 从延迟链表移除。
 *
 * @note 仅 TASK_BLOCKED 状态处理。
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
 * @brief 按 wake_tick 插入全局延迟唤醒链表。
 *
 * @param task      任务指针。
 * @param wake_tick 唤醒时刻（g_ticks 基准）。
 *
 * @details
 * 1. 设置 task->wake_tick = wake_tick。
 * 2. 调用 list_insert_sorted 按 wake_tick 升序插入 g_delayed_list。
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
 * @brief 阻塞当前任务并可选加入延迟唤醒。
 *
 * @param task    任务指针。
 * @param reason  阻塞原因。
 * @param obj     关联对象（IPC 等）。
 * @param timeout 超时 tick；portMAX_DELAY 无超时；0 立即超时点。
 *
 * @details
 * 1. task 为空或 idle（id==0）则返回。
 * 2. 若 task 当前 READY，先从就绪队列移除。
 * 3. 设置 state=BLOCKED、block_reason、block_obj。
 * 4. 按 timeout 分支：
 *    a. portMAX_DELAY：wake_tick=0，从延迟链移除；
 *    b. timeout==0：wake_tick=g_ticks，立即插入延迟链；
 *    c. 否则：插入 g_ticks+timeout 时刻的延迟链。
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
 * @brief 阻塞任务直到绝对 wake_tick（g_ticks 基准）。
 *
 * @param task      任务指针。
 * @param reason    阻塞原因。
 * @param obj       关联对象。
 * @param wake_tick 绝对唤醒时刻；若已到期则不阻塞（调用方应先判断）。
 *
 * @details
 * 1. task 为空或 idle 则返回。
 * 2. 若 READY 则先从就绪队列移除。
 * 3. 设置 BLOCKED 状态及 block_reason、block_obj。
 * 4. 调用 sched_delayed_insert 按绝对 wake_tick 插入延迟链。
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
 * @brief 唤醒阻塞任务并加入就绪队列。
 *
 * @param task 任务指针。
 * @return pdPASS 成功；pdFAIL 非 BLOCKED 状态。
 *
 * @details
 * 1. task 为空或 state != TASK_BLOCKED 则返回 pdFAIL。
 * 2. 从延迟链移除，清除 block_reason、block_obj、wake_tick。
 * 3. 调用 cgrtos_sched_add_ready 加入就绪队列。
 * 4. SMP：若目标核 != 本核且次核已上线，向目标 hart 发 IPI 唤醒调度。
 *
 * @note 跨核目标时向 dst hart 发送 IPI。
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
 * @brief 挂起调度器（嵌套计数 +1）。
 *
 * @details
 * 1. 进入临界区。
 * 2. g_sched_suspend_count 递增。
 * 3. 退出临界区。
 *
 * @note 挂起期间不进行上下文切换。
 */
void cgrtos_sched_suspend(void)
{
    cgrtos_enter_critical();
    g_sched_suspend_count++;
    cgrtos_exit_critical();
}

/**
 * @brief 恢复调度器；嵌套归零时触发 yield。
 *
 * @details
 * 1. 进入临界区，若 suspend_count > 0 则递减。
 * 2. 退出临界区。
 * 3. 若 suspend_count 归零，调用 cgrtos_sched_yield 触发 pending 切换。
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
 * @brief 主动让出 CPU（任务上下文 ecall 进入 trap 切换）。
 *
 * @details
 * 1. 若调度挂起或未运行，直接返回。
 * 2. 置本核 g_yield_pending 与 g_force_yield 标志。
 * 3. 若在 ISR 内，仅置标志，trap 返回路径完成切换。
 * 4. 否则执行 ecall 进入 trap_vector，在返回前完成 switch_from_trap。
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
 * @brief 从中断上下文请求调度切换。
 *
 * @details
 * 1. 读取本核编号 cpu。
 * 2. 置 g_yield_pending[cpu]=1，在 trap 返回路径完成切换。
 */
void cgrtos_sched_yield_from_isr(void)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    g_yield_pending[cpu] = 1;
}

/**
 * @brief 陷阱返回路径上的上下文切换核心逻辑。
 *
 * @param sp 当前任务陷阱栈指针。
 * @return 下一任务应恢复的 sp；未切换时返回原 sp。
 *
 * @details
 * 1. 若调度挂起或未运行，返回原 sp。
 * 2. 保存当前任务 sp 到 cur->sp。
 * 3. 关 IRQ 并持 g_ready_lock，调用 sched_pick_next 选取 next。
 * 4. 粘性调度判断（PRIORITY/Hybrid RT）：
 *    a. 非 forced yield 且策略为 PRIORITY/Hybrid RT；
 *    b. 若无更好任务、或 next 为 CFS、或 next 优先级不高于 cur、或 EDF 未临近 deadline；
 *    c. 则 stick=1，解锁返回原 sp（不换出）。
 * 5. 若不 stick：cur 置 READY 并 requeue（CFS 在此累计 vruntime 记账已在 tick_local 完成）。
 * 6. next 无 sp 则回退 idle；否则从就绪队列 dequeue。
 * 7. 若 cur != next：更新 g_current、next 状态/statistics，解锁，返回 next->sp。
 * 8. 若相同任务：恢复 cur 为 RUNNING，解锁，返回原 sp。
 *
 * @note 处理 PRIORITY 粘性、CFS vruntime、强制 yield 及 idle 回退。
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

        /* 已离开 DELETED 任务：现在可安全清 id */
        if (cur && cur->state == TASK_DELETED) {
            cgrtos_task_reclaim_deleted(cur);
        }

        cgrtos_spin_unlock(&g_ready_lock);
        cgrtos_irq_restore(flags);

#if CONFIG_CHECK_STACK_OVERFLOW
        /* 切换切入前检查下一任务金丝雀 */
        if (next->id > 0 && next->stack[0] != STACK_CANARY_VALUE) {
            cgrtos_task_handle_stack_overflow(next);
        }
#endif
        return next->sp;
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
 * @brief 处理到期延迟唤醒（超时 unblock）。
 *
 * @details
 * 1. 进入临界区（与 IPC block/unblock 串行化，持 g_klock）。
 * 2. 循环检查 g_delayed_list 队头：
 *    a. 队空或 wake_tick > g_ticks 则退出；
 *    b. 移除队头 delayed_item，反算 task 指针；
 *    c. 若 task 非 BLOCKED 则跳过；
 *    d. 置 wake_ok=0（超时），purge 等待队列，unblock 唤醒。
 * 3. 退出临界区。
 *
 * @note 由 hart0 tick 调用；wake_ok=0 表示超时。
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
 * @brief 本核时间片记账与抢占请求（不含全局 g_ticks）。
 *
 * @details
 * 1. 读取本核当前任务 cur。
 * 2. 若 cur 非 idle：exec 计数递增；CFS 累加 vruntime；slice_remain 递减。
 * 3. 时间片耗尽判断：RR/CFS/EDF 且 RUNNING 时刷新 slice 并 need_yield=1；PRIORITY 仅重置 slice。
 * 4. 调度未挂起时 always need_yield=1（新唤醒高优先级可抢占）。
 * 5. ISR 内置 yield_pending；任务上下文直接 cgrtos_sched_yield。
 *
 * @note hart0 由硬定时器调用；次核由 g_remote_tick + IPI 调用。
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
 * @brief 系统 tick 中断处理（每核部分 + hart0 全局工作）。
 *
 * @details
 * 1. 读取本核 cpu 编号。
 * 2. hart0 专属全局工作：
 *    a. 原子递增 g_ticks；
 *    b. sched_process_delayed_tasks 处理延迟超时；
 *    c. sched_process_edf_releases 处理 EDF 释放轮；
 *    d. cgrtos_timer_process_tick 软件定时器；
 *    e. 可选 tick hook；
 *    f. 周期性负载均衡 cgrtos_sched_load_balance；
 *    g. SMP：向次核置 g_remote_tick 并发 IPI。
 * 3. 所有核调用 cgrtos_tick_local 做本地时间片记账。
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
 * @brief 统计指定核上就绪任务总数。
 *
 * @param cpu 核编号。
 * @return 就绪任务数；cpu 越界时返回 0。
 *
 * @details
 * 1. cpu 越界返回 0。
 * 2. 累加所有优先级 ready_list 的 count。
 * 3. 加上 CFS 与 EDF 链表节点数。
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
 * @brief 计算任务在负载均衡中的权重。
 *
 * @param task 任务指针。
 * @return 权重值；EDF/idle 返回 0（不可迁移或不计入）。
 *
 * @details
 * 1. task 为空或 idle 返回 0。
 * 2. EDF 任务返回 0（MC-EDF 自行占核，不参与加权 LB）。
 * 3. CFS 任务返回 CONFIG_LB_CFS_WEIGHT。
 * 4. RT 任务返回 (prio+1) * CONFIG_LB_PRIO_SCALE。
 *
 * @note 权重越高表示迁移成本越大或越应保留本地。
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
 * @brief 判断 READY 任务是否可被迁移到其它核。
 *
 * @param task 任务指针。
 * @return 非零表示可迁移；0 表示不可迁移。
 *
 * @details
 * 1. task 为空、idle 或非 READY 返回 0。
 * 2. cpu_aff 硬亲和（!= 0xFF）返回 0。
 * 3. EDF 任务返回 0（由 MC-EDF 全局分配，禁止 LB 乱迁）。
 * 4. 优先级 >= TIMER_TASK_PRIO-1（定时器守护/近顶 RT）返回 0。
 * 5. 其余返回 1。
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
 * @brief 计算指定核的加权负载。
 *
 * @param cpu 核编号。
 * @return 加权负载和；含 READY（目标核为本核）与当前 RUNNING 非 idle 任务。
 *
 * @details
 * 1. cpu 越界返回 0。
 * 2. 遍历 g_tasks[]：state==READY 且 target_core==cpu 的任务累加 weight。
 * 3. 当前 RUNNING 非 idle 任务也累加 weight。
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
 * @brief 返回负载最低的可运行核（SMP）。
 *
 * @return 核编号；单核或未上线 secondary 时返回 0。
 *
 * @details
 * 1. 单核配置直接返回 0。
 * 2. 次核未上线返回 0。
 * 3. 比较 l0 与 l1，返回负载较低者。
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
 * @brief 将 READY 任务从当前分配迁移到 dst 并发送 IPI。
 *
 * @param task 任务指针。
 * @param dst  目标核编号。
 *
 * @details
 * 1. 从就绪队列 remove_ready。
 * 2. 更新 task->run_cpu = dst。
 * 3. add_ready 到目标核队列。
 * 4. 向 dst 发 IPI，递增 g_lb_migrate_count。
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
 * @brief 在 busy 核上选取最佳迁移牺牲者（低权重、高 vruntime 优先）。
 *
 * @param src 源核编号。
 * @return 任务指针；无可迁移任务时返回 NULL。
 *
 * @details
 * 1. 遍历 g_tasks[]，筛选 migratable 且 target_core==src 的任务。
 * 2. 在候选中选 weight 最小者；weight 相同时选 vruntime 较大者。
 * 3. 返回最佳 victim 或 NULL。
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
 * @brief 加权 push 负载均衡（hart0 周期性调用）。
 *
 * @details
 * 1. 调度未运行或挂起则返回；单核或次核未上线则返回。
 * 2. 计算 l0、l1，确定 busy 核与 idle 核，计算负载差 diff。
 * 3. 当 diff > HYST 且 moved < 4：
 *    a. 在 busy 核选 victim（sched_pick_victim）；
 *    b. 无 victim 则退出；
 *    c. sched_migrate_task 迁移到 idle 核；
 *    d. 重新计算 diff，继续循环。
 *
 * @note 比较两核加权负载，将 busy 核上最低紧急度 READY 任务 push 到 idle 核。
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
 * @brief Idle 任务中的工作窃取（从 peer 核 pull 任务）。
 *
 * @details
 * 1. CONFIG_SMP_IDLE_STEAL 未启用或单核则返回。
 * 2. 调度未运行、次核未上线或挂起则返回。
 * 3. 本核已有就绪任务则返回（不窃取）。
 * 4. 对端就绪数 < CONFIG_LB_STEAL_MIN 则返回。
 * 5. 从对端选 victim，迁移到本核，递增 steal 计数，置 yield_pending。
 *
 * @note 受 CONFIG_SMP_IDLE_STEAL 控制；本地已有工作或 peer 就绪数不足时不窃取。
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
 * @brief 初始化调度器内部数据结构。
 *
 * @details
 * 1. 清零 g_ready 与 g_ready_bitmap。
 * 2. 初始化每核 CFS 就绪链表与全局 MC-EDF 就绪链。
 * 3. 初始化 EDF 释放轮各槽位链表。
 * 4. 初始化 g_delayed_list，edf_rel_cursor=0。
 * 5. suspend_count=0，ready_lock=0，lb_tick=0，force_yield 清零。
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
