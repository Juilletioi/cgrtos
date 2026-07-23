/**
 * @file task.c
 * @brief 任务生命周期、通知、延迟与 idle
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details 提供任务创建/删除/挂起/恢复、优先级与亲和性、
 *          周期/截止时间、通知、延迟及 per-core idle 任务实现。
 *
 *          任务栈内嵌于 TCB（CONFIG_TASK_STACK_WORDS）。
 *          亲和性：cpu_aff==0xFF 表示可迁移；否则固定到指定 hart。
 *          Idle：可选 busy mtime 泵（QEMU）；执行工作窃取。
 */
#include "cgrtos.h"
#include <string.h>

/** @brief 下一个分配的任务 ID（单调递增） */
static uint64_t g_next_id = 1;

/** @def STACK_CANARY 栈底金丝雀值，用于溢出检测 */
#define STACK_CANARY  STACK_CANARY_VALUE

/** @brief 累计任务创建次数（统计） */
uint32_t g_task_create_count;
/** @brief 累计任务删除次数（统计） */
uint32_t g_task_delete_count;

#if CONFIG_USE_HOOKS
extern cgrtos_hook_fn_t g_idle_hook;
#if CONFIG_IDLE_SLEEP_HOOK
cgrtos_hook_fn_t g_idle_sleep_hook;
/**
 * @brief 注册 idle 睡眠前钩子函数
 * @details 将全局 g_idle_sleep_hook 设为 hook；在 idle 任务进入 WFI/忙等前调用。
 * @param[in] hook 钩子回调；NULL 表示清除
 * @return 无
 * @retval 无
 * @note 钩子须短小非阻塞
 * @warning 钩子内阻塞会延迟 idle 循环与工作窃取
 * @attention ❌ ISR；❌ 可能阻塞或上下文切换
 */
void cgrtos_set_idle_sleep_hook(cgrtos_hook_fn_t hook)
{
    g_idle_sleep_hook = hook;
}
#endif
#endif

/**
 * @brief 任务入口 trampoline：调用用户函数后强制 exit
 * @details 取本核 current，调用 entry_fn(entry_arg)，随后 cgrtos_task_exit 闭环 TERMINATED。
 * @param[in] arg 未使用（参数经 TCB entry_arg 传递）
 * @return 无（经 cgrtos_task_exit 永不返回）
 * @retval 无
 * @note mepc 指向本函数；禁止用户函数 return 后跑飞
 * @warning 无
 * @attention @internal
 */
static void task_bootstrap(void *arg)
{
    (void)arg;
    uint8_t cpu = arch_cpu_id();
    cgrtos_task_t *self = g_current[cpu];
    if (self && self->entry_fn) {
        self->entry_fn(self->entry_arg);
    }
    cgrtos_task_exit();
}

/**
 * @brief 初始化任务栈帧，使其可从 context_switch 首次启动
 * @details 委托 arch_task_stack_init（RISC-V trap 帧 / ARM64 异常帧）。
 * @param[in] task 目标任务 TCB
 * @param[in] fn   任务入口
 * @param[in] arg  入口参数
 * @return 初始栈指针
 * @retval 非 NULL
 * @attention @internal
 */
static uint64_t *task_init_stack(cgrtos_task_t *task, task_func_t fn, void *arg)
{
    arch_task_stack_init(task, fn, arg);
    return task->sp;
}

/**
 * @brief 在 g_tasks 数组中分配可复用的空闲 TCB 槽位
 * @details 线性扫描 id==0 或 state==DELETED/TERMINATED 的槽；确认无核 g_current 仍引用后返回。
 * @return 可用 TCB 指针；任务表满或候选槽仍被引用时返回 NULL
 * @retval 非 NULL 可复用 TCB
 * @retval NULL    无可用槽或存在 use-after-free 竞态
 * @note 调用方须已持临界区
 * @warning 跳过仍被 g_current 引用的槽以防 UAF
 * @attention @internal
 */
static cgrtos_task_t *task_alloc_slot(void)
{
    for (uint32_t i = 0; i < CONFIG_MAX_TASKS; i++) {
        cgrtos_task_t *t = &g_tasks[i];
        /* 1. 跳过仍被占用的有效任务槽 */
        if (t->id != 0 && t->state != TASK_DELETED &&
            t->state != TASK_TERMINATED) {
            continue;
        }
        /* 2. 检查是否仍被任意核作为当前运行任务引用 */
        int live = 0;
        for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
            if (g_current[c] == t) {
                live = 1;
                break;
            }
        }
        if (!live) {
            return t;
        }
    }
    return 0;
}

/**
 * @brief 按单调递增的任务 ID 在 g_tasks 中查找 TCB
 * @details 线性扫描 g_tasks 全表，比较 task->id 与目标 id。
 * @param[in] id 创建时分配的任务 ID（g_next_id 序列）
 * @return 匹配的 TCB 指针；未找到返回 NULL
 * @retval 非 NULL 找到
 * @retval NULL    未找到
 * @note 无锁查找，SMP 下存在撕裂风险
 * @warning 无
 * @attention @internal
 */
static cgrtos_task_t *task_find(task_id_t id)
{
    for (uint32_t i = 0; i < CONFIG_MAX_TASKS; i++) {
        if (g_tasks[i].id == id) {
            return &g_tasks[i];
        }
    }
    return 0;
}

/**
 * @brief 将内核内部 task_state_t 映射为对外 API 枚举 eTaskState_t
 * @details 按 switch-case 映射 READY/RUNNING/BLOCKED/SUSPENDED/DELETED/TERMINATED；未知返回 eInvalid。
 * @param[in] state 内部任务状态
 * @return 对应的 eTaskState_t
 * @retval eReady/eRunning/eBlocked/eSuspended/eDeleted/eTerminated 对应状态
 * @retval eInvalid 未知状态
 * @note 无
 * @warning 无
 * @attention @internal
 */
static eTaskState_t task_state_to_enum(task_state_t state)
{
    switch (state) {
    case TASK_READY:    return eReady;
    case TASK_RUNNING:  return eRunning;
    case TASK_BLOCKED:  return eBlocked;
    case TASK_SUSPENDED: return eSuspended;
    case TASK_TERMINATED: return eTerminated;
    case TASK_DELETED:  return eDeleted;
    default:            return eInvalid;
    }
}

/**
 * @brief 创建新任务并加入就绪队列
 * @details 从 TCB 池分配槽，初始化栈（经 bootstrap）、策略与亲和性，加入就绪结构；
 *          SMP 下可能向目标核发 IPI。
 * @param[in] name   任务名（可为 NULL，默认 "task"）
 * @param[in] fn     入口函数，不可为 NULL
 * @param[in] arg    传递给入口的 void* 参数
 * @param[in] prio   优先级；超过 CONFIG_MAX_PRIORITY 则截断至最大值
 * @param[in] policy 调度策略（RR/PRIORITY/CFS/EDF/HYBRID）
 * @return 新任务 ID；参数无效、任务表满或槽位分配失败时返回 (task_id_t)-1
 * @retval >0           成功
 * @retval (task_id_t)-1 参数非法 / 池满 / 策略禁用
 * @note 任务入口返回后经 bootstrap 调用 cgrtos_task_exit
 * @warning 禁止应用直接改写返回 ID 对应 TCB 字段
 * @attention ❌ ISR；❌ 通常不阻塞调用者（可能 IPI 触发他核调度）
 */
task_id_t cgrtos_task_create(const char *name, task_func_t fn, void *arg,
                             uint8_t prio, sched_policy_t policy)
{
    /* 1. 校验 fn 非空并截断 prio */
    if (!fn) {
        return (task_id_t)-1;
    }
#if !CONFIG_USE_EDF
    if (policy == SCHED_EDF) {
        return (task_id_t)-1;
    }
#endif
    if (prio > CONFIG_MAX_PRIORITY) {
        prio = CONFIG_MAX_PRIORITY;
    }

    cgrtos_enter_critical();

    /* 2. 检查任务表是否已满 */
    if (g_task_count >= CONFIG_MAX_TASKS) {
        cgrtos_exit_critical();
        return (task_id_t)-1;
    }

    /* 3. 分配可复用 TCB 槽位 */
    cgrtos_task_t *task = task_alloc_slot();
    if (!task) {
        cgrtos_exit_critical();
        return (task_id_t)-1;
    }

    /* 4. 初始化 TCB 字段与调度链表节点 */
    memset(task, 0, sizeof(*task));
    list_init_item(&task->delayed_item);
    list_init_item(&task->cfs_item);
    list_init_item(&task->edf_item);
    list_init_item(&task->edf_rel_item);
    task->edf_on_wheel = 0;
    task->id = g_next_id++;
    if (g_next_id == 0) {
        g_next_id = 1; /* 避免 id 回绕为 0（0 表示空槽） */
    }
    g_task_count++;

    /* 5. 设置名称、优先级、策略与亲和性 */
    strncpy(task->name, name ? name : "task", CGRTOS_TASK_NAME_MAX - 1);
    task->name[CGRTOS_TASK_NAME_MAX - 1] = 0;
    task->prio = prio;
    task->base_prio = prio;
#if CONFIG_USE_PREEMPT_THRESH
    task->preempt_thresh = prio; /* 默认与 prio 相同 = 经典抢占 */
#endif
    task->policy = policy;
    task->entry_fn = fn;
    task->entry_arg = arg;
    task->cpu_aff = 0xFF;
#if CONFIG_SMP_INITIAL_PLACE
    task->run_cpu = cgrtos_sched_least_loaded_core();
#else
    task->run_cpu = arch_cpu_id();
#endif
    task->slice_remain = CONFIG_TIME_SLICE_TICKS;
    /* 6. 构建 trap 帧（bootstrap）并加入就绪队列 */
    task->sp = task_init_stack(task, task_bootstrap, 0);
    task->last_run = g_ticks;
#if CONFIG_SCHED_STATS
    task->ready_since = g_ticks;
#endif
#if CONFIG_USE_EDF
    /* 创建时 deadline=now，首轮即可参与紧迫窗口；set_period 后再对齐周期 */
    if (policy == SCHED_EDF) {
        task->deadline = g_ticks;
    }
#endif

    cgrtos_sched_add_ready(task);
    g_task_create_count++;
    cgrtos_exit_critical();

#if CONFIG_USE_HOOKS
    {
        extern cgrtos_hook_fn_t g_task_create_hook;
        if (g_task_create_hook) {
            g_task_create_hook();
        }
    }
#endif

    /* 7. 对端核需 IPI 才能调度新就绪任务 */
#if CONFIG_NUM_CORES > 1
    if (CGRTOS_CORE_ONLINE(task->run_cpu) &&
        task->run_cpu != arch_cpu_id()) {
        cgrtos_smp_send_ipi(task->run_cpu);
    }
#endif

    /* 8. 非临时任务打印创建日志并返回 id */
    if (!(name && name[0] == 't' && name[1] == 'm' && name[2] == 'p' &&
          name[3] == '\0')) {
        cgrtos_printf("  [TASK] create '%s' id=%lu prio=%u policy=%d\n",
                      task->name, task->id, task->prio, policy);
    }
    return task->id;
}

/**
 * @brief 删除指定任务（安全：禁删 idle、释锁、延迟回收槽）
 * @details 释放其持有互斥量、摘就绪/等待队列、置 DELETED；若仍在他核运行则 IPI 后延迟 reclaim。
 * @param[in] id 待删除任务 ID
 * @return pdPASS 成功；任务不存在 / 试图删 idle 返回 pdFAIL
 * @retval pdPASS 成功标记删除
 * @retval pdFAIL 任务不存在或试图删除 idle
 * @note 自删除会 yield 切走；仍运行时不立即清 id
 * @warning 删除仍持锁任务依赖 force_release
 * @attention ❌ ISR；✅ 可能触发调度（自删/同核 RUNNING）
 */
int cgrtos_task_delete(task_id_t id)
{
    /* 1. 进入临界区并按 id 查找 TCB */
    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    if (!task || task->id == 0) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    /* 2. 禁止删除 idle */
    for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
        if (task == &g_idle[c]) {
            cgrtos_exit_critical();
            return pdFAIL;
        }
    }

    uint8_t self = arch_cpu_id();
    uint8_t run = task->run_cpu;

    /* 3. 释放该任务持有的互斥量（避免等待者死锁）
     *    force_release 自带临界区嵌套，此处先临时退出再进入以保持清晰——
     *    实际上 nest 安全，直接调用即可。 */
    cgrtos_exit_critical();
    cgrtos_mutex_force_release_owned(task);
    cgrtos_enter_critical();

    /* 重新确认（竞态窗口极短；id 仍匹配则继续） */
    if (task->id != id) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    /* 4. 从就绪队列或阻塞等待链中摘除 */
    if (task->state == TASK_READY) {
        cgrtos_sched_remove_ready(task);
    } else if (task->state == TASK_BLOCKED) {
        cgrtos_task_purge_waits(task);
    }

    /* 5. 清除 EDF 周期并解除时间轮挂载 */
    task->period = 0;
    cgrtos_sched_edf_arm(task);

    /* 6. 标记 DELETED 并更新计数（暂不清 id） */
    task->state = TASK_DELETED;
    if (g_task_count > 0) {
        g_task_count--;
    }
    g_task_delete_count++;

    /* 7. 若已不在任何核上运行，立即回收槽；否则留给 reclaim */
    int still_running = 0;
    for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
        if (g_current[c] == task) {
            still_running = 1;
            break;
        }
    }
    if (!still_running) {
        task->id = 0;
    }

    /* 8. 远程核仍运行被删 TCB 时必须 kick */
    if (run < CONFIG_NUM_CORES && run != self && g_current[run] == task) {
        cgrtos_smp_send_ipi(run);
    }
    cgrtos_exit_critical();

#if CONFIG_USE_HOOKS
    {
        extern cgrtos_hook_fn_t g_task_delete_hook;
        if (g_task_delete_hook) {
            g_task_delete_hook();
        }
    }
#endif

    /* yield：自删或同核 RUNNING 时尽快切走 */
    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 切换离开已删除任务后回收 TCB（清 id）
 * @details task 为空或 state 非 DELETED 则返回；仍被任意核 g_current 引用则暂不回收，否则 id=0。
 * @param[in] task 可能为 TASK_DELETED 的 TCB
 * @return 无
 * @retval 无
 * @note 由调度器上下文切换路径调用
 * @warning 过早清 id 将导致槽位复用 UAF
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_task_reclaim_deleted(cgrtos_task_t *task)
{
    if (!task || task->state != TASK_DELETED) {
        return;
    }
    for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
        if (g_current[c] == task) {
            return;
        }
    }
    task->id = 0;
}

/**
 * @brief 检查任务栈金丝雀
 * @details 查找 TCB；CONFIG_CHECK_STACK_OVERFLOW 关闭时直接 pdPASS；否则比较 stack[0] 与金丝雀值。
 * @param[in] id 任务 ID
 * @return pdPASS 完好；pdFAIL 溢出/无效
 * @retval pdPASS 栈底金丝雀完好
 * @retval pdFAIL 溢出或无效任务
 * @note 需 CONFIG_CHECK_STACK_OVERFLOW 才有实际检测
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_task_check_stack(task_id_t id)
{
    cgrtos_task_t *task = task_find(id);
    if (!task || task->id == 0) {
        return pdFAIL;
    }
#if CONFIG_CHECK_STACK_OVERFLOW
    return (task->stack[0] == STACK_CANARY_VALUE) ? pdPASS : pdFAIL;
#else
    (void)task;
    return pdPASS;
#endif
}

/**
 * @brief 栈溢出统一处理：计数 → 钩子 → 默认断言停机
 * @details 递增 g_stack_overflow_count；若注册钩子则调用；打印任务名并 cgrtos_assert_failed 停机。
 * @param[in] task 溢出任务（可空）
 * @return 无（不返回）
 * @retval 无
 * @note 调度器切换 / tick 抽检路径调用
 * @warning 默认路径将 halt 系统
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_task_handle_stack_overflow(cgrtos_task_t *task)
{
    extern uint32_t g_stack_overflow_count;
    extern cgrtos_stack_overflow_hook_t g_stack_overflow_hook;

    g_stack_overflow_count++;
    if (g_stack_overflow_hook) {
        g_stack_overflow_hook(task);
    }
    cgrtos_printf("[STACK] overflow in '%s'\n",
                  (task && task->name[0]) ? task->name : "?");
    cgrtos_assert_failed("stack_overflow", 0);
}

/**
 * @brief 查询任务累计运行 tick（exec）
 * @details 线性查找 TCB 并返回 exec 字段。
 * @param[in] id 任务 ID
 * @return exec；无效任务返回 0
 * @retval >=0 累计运行 tick
 * @retval 0     无效任务
 * @note 无锁快照，SMP 下可能略有滞后
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
tick_t cgrtos_task_get_runtime(task_id_t id)
{
    cgrtos_task_t *task = task_find(id);
    if (!task || task->id == 0) {
        return 0;
    }
    return task->exec;
}

/**
 * @brief 挂起指定任务，使其不再参与调度
 * @details 从就绪队列移除并置 SUSPENDED；对 RUNNING 目标会请求重调度。
 * @param[in] id 任务 ID
 * @return pdPASS 成功；任务不存在返回 pdFAIL
 * @retval pdPASS 成功
 * @retval pdFAIL 无效 ID
 * @note 可用 resume 恢复
 * @warning 挂起持有互斥量的任务可能导致优先级反转/死锁
 * @attention ❌ ISR；✅ 可能引起切换
 */
int cgrtos_task_suspend(task_id_t id)
{
    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    if (!task) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    if (task->state == TASK_READY) {
        cgrtos_sched_remove_ready(task);
    } else if (task->state == TASK_BLOCKED) {
        cgrtos_task_purge_waits(task);
    }
    task->state = TASK_SUSPENDED;
    cgrtos_exit_critical();

    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 恢复此前被挂起的任务
 * @details 置 READY 并入队；退出临界区后 yield，使高优先级就绪任务有机会运行。
 * @param[in] id 任务 ID
 * @return pdPASS 成功；任务不存在或当前非 SUSPENDED 返回 pdFAIL
 * @retval pdPASS 成功
 * @retval pdFAIL 无效或不在挂起态
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能引起切换
 */
int cgrtos_task_resume(task_id_t id)
{
    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    if (!task || task->state != TASK_SUSPENDED) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    cgrtos_sched_add_ready(task);
    cgrtos_exit_critical();
    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 修改任务调度优先级
 * @details READY 时先 remove 再改 prio/base_prio 再 add_ready；非 READY 仅更新字段；最后 yield。
 * @param[in] id   任务 ID
 * @param[in] prio 新优先级（0..CONFIG_MAX_PRIORITY）
 * @return pdPASS 成功；prio 越界或任务不存在返回 pdFAIL
 * @retval pdPASS 成功
 * @retval pdFAIL 越界或任务不存在
 * @note 与 PI/DPCP 共存时有效 prio 可能仍被抬升
 * @warning 降低持锁任务优先级可能加剧优先级反转窗口
 * @attention ❌ ISR；✅ 可能 yield
 */
int cgrtos_task_set_priority(task_id_t id, uint8_t prio)
{
    if (prio > CONFIG_MAX_PRIORITY) {
        return pdFAIL;
    }

    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    if (!task) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    if (task->state == TASK_READY) {
        cgrtos_sched_remove_ready(task);
    }

    task->prio = prio;
    task->base_prio = prio;
#if CONFIG_USE_PREEMPT_THRESH
    if (task->preempt_thresh < prio) {
        task->preempt_thresh = prio;
    }
#endif

    if (task->state == TASK_READY) {
        cgrtos_sched_add_ready(task);
    }
    cgrtos_exit_critical();

    cgrtos_sched_yield();
    return pdPASS;
}

#if CONFIG_USE_PREEMPT_THRESH
/**
 * @brief 设置抢占阈值
 * @details 查 TCB；thresh 须 >= base_prio 且 <= MAX；写入 preempt_thresh 后 yield。
 * @param[in] id     任务 ID
 * @param[in] thresh 阈值；须 >= base_prio 且 <= CONFIG_MAX_PRIORITY
 * @return pdPASS 成功；参数非法或任务不存在返回 pdFAIL
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或任务不存在
 * @note 默认 create 时 thresh=prio（经典抢占）
 * @warning 阈值过高会推迟高优先级响应，仅用于短临界段降抖动
 * @attention ❌ ISR；✅ 可能 yield
 */
int cgrtos_task_set_preempt_threshold(task_id_t id, uint8_t thresh)
{
    if (thresh > CONFIG_MAX_PRIORITY) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    if (!task) {
        cgrtos_exit_critical();
        return pdFAIL;
    }
    if (thresh < task->base_prio) {
        cgrtos_exit_critical();
        return pdFAIL;
    }
    task->preempt_thresh = thresh;
    cgrtos_exit_critical();
    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 读取任务抢占阈值
 * @details 临界区内读取 preempt_thresh 字段。
 * @param[in] id 任务 ID
 * @return 阈值数值；无效任务返回 0
 * @retval 0..CONFIG_MAX_PRIORITY 有效阈值
 * @retval 0 亦可能表示无效任务
 * @note 无
 * @warning 无
 * @attention ❌ ISR（持 g_klock）；❌ 不阻塞
 */
uint8_t cgrtos_task_get_preempt_threshold(task_id_t id)
{
    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    uint8_t v = 0;
    if (task) {
        v = task->preempt_thresh;
    }
    cgrtos_exit_critical();
    return v;
}
#endif /* CONFIG_USE_PREEMPT_THRESH */

/**
 * @brief 当前任务正常退出（TERMINATED→DELETED）
 * @details 置 TERMINATED 后调用 delete(self)；之后死循环 yield。bootstrap 在入口返回时自动调用。
 * @return 无（永不返回）
 * @retval 无
 * @note idle 禁止退出
 * @warning 在持有互斥量时退出依赖 delete 的 force_release
 * @attention ❌ ISR；✅ 必定引起调度
 */
void cgrtos_task_exit(void)
{
    uint8_t cpu = arch_cpu_id();
    cgrtos_task_t *cur = g_current[cpu];
    if (!cur || cur->id == 0) {
        /* 无效 current：不可空转占核；强制挂起本任务上下文 */
        while (1) {
            cgrtos_task_yield();
        }
    }
    for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
        if (cur == &g_idle[c]) {
            while (1) {
                cgrtos_task_yield();
            }
        }
    }
    cur->state = TASK_TERMINATED;
    (void)cgrtos_task_delete(cur->id);
    while (1) {
        cgrtos_task_yield();
    }
}

#if CONFIG_SCHED_STATS
/**
 * @brief 查询任务调度统计快照
 * @details 临界区内填充 max/last/latency_sum/samples、exec_ticks 与 cpu_util_permille。
 * @param[in]  id  任务 ID
 * @param[out] out 输出结构；不可为 NULL
 * @return pdPASS 成功；参数或任务无效返回 pdFAIL
 * @retval pdPASS 成功
 * @retval pdFAIL out 为 NULL 或任务不存在
 * @note 需 CONFIG_SCHED_STATS 编译启用
 * @warning 无
 * @attention ❌ ISR（持 g_klock）；❌ 不阻塞
 */
int cgrtos_task_get_sched_stats(task_id_t id, cgrtos_task_sched_stats_t *out)
{
    if (!out) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    if (!task) {
        cgrtos_exit_critical();
        return pdFAIL;
    }
    out->max_latency = task->max_sched_latency;
    out->last_latency = task->last_sched_latency;
    out->latency_sum = task->sched_latency_sum;
    out->latency_samples = task->sched_latency_samples;
    out->exec_ticks = task->exec;
    if (g_ticks > 0) {
        out->cpu_util_permille =
            (uint32_t)((task->exec * 1000ULL) / (uint64_t)g_ticks);
    } else {
        out->cpu_util_permille = 0;
    }
    cgrtos_exit_critical();
    return pdPASS;
}
#endif

/**
 * @brief 设置任务 CPU 亲和性（硬绑定或可迁移）
 * @details 0xFF 表示可迁移；否则硬绑定到指定 hart；READY 时重入队；绑定非本核则 IPI 后 yield。
 * @param[in] id  任务 ID
 * @param[in] cpu 目标核编号；0xFF 表示无亲和性限制、可负载均衡迁移
 * @return pdPASS 成功；cpu 非法、次核离线或任务不存在返回 pdFAIL
 * @retval pdPASS 成功
 * @retval pdFAIL 无效任务或 cpu 非法/次核离线
 * @note 可能伴随迁移 IPI
 * @warning 硬绑满核可能导致过载无法迁移
 * @attention ❌ ISR；✅ 可能引起切换
 */
int cgrtos_task_set_affinity(task_id_t id, uint8_t cpu)
{
    if (cpu != 0xFF && cpu >= CONFIG_NUM_CORES) {
        return pdFAIL;
    }
    /* 次核尚未 online 时不允许硬亲和到该核 */
    if (cpu != 0xFF && cpu != 0 && !CGRTOS_CORE_ONLINE(cpu)) {
        return pdFAIL;
    }

    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    if (!task) {
        cgrtos_exit_critical();
        return pdFAIL;
    }

    if (task->state == TASK_READY) {
        cgrtos_sched_remove_ready(task);
    }

    task->cpu_aff = cpu;

    if (task->state == TASK_READY) {
        cgrtos_sched_add_ready(task);
    }
    cgrtos_exit_critical();

    if (cpu != 0xFF && cpu != arch_cpu_id()) {
        cgrtos_smp_send_ipi(cpu);
    }
    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 设置 EDF 任务的周期（tick）
 * @details 写入 period；若策略为 SCHED_EDF 且 period>0，设置 deadline=g_ticks+period 并挂载 EDF 时间轮。
 * @param[in] id     任务 ID
 * @param[in] period 周期 tick 数；0 表示清除周期
 * @return pdPASS 成功；任务不存在或 CONFIG_USE_EDF=0 返回 pdFAIL
 * @retval pdPASS 成功
 * @retval pdFAIL 任务不存在或 EDF 未启用
 * @note 仅对 SCHED_EDF 有意义
 * @warning 过短周期可能导致持续过载与错过 deadline
 * @attention ❌ ISR；❌ 通常不阻塞
 */
int cgrtos_task_set_period(task_id_t id, tick_t period)
{
#if !CONFIG_USE_EDF
    (void)id;
    (void)period;
    return pdFAIL;
#else
    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    if (!task) {
        cgrtos_exit_critical();
        return pdFAIL;
    }
    task->period = period;
    if (task->policy == SCHED_EDF && period > 0) {
        task->deadline = g_ticks + period;
        cgrtos_exit_critical();
        cgrtos_sched_edf_arm(task);
        return pdPASS;
    }
    cgrtos_exit_critical();
    return pdPASS;
#endif
}

/**
 * @brief 设置任务的绝对截止时间（tick）
 * @details 更新 deadline；若任务 READY 则按新键重入就绪结构；period>0 时同步释放轮。
 * @param[in] id       任务 ID
 * @param[in] deadline 绝对 deadline（相对 g_ticks 的时间点）
 * @return pdPASS 成功；任务不存在返回 pdFAIL
 * @retval pdPASS 成功
 * @retval pdFAIL 任务不存在
 * @note 无
 * @warning deadline 已过期时仍可能被选中运行（取决于 pick 规则）
 * @attention ❌ ISR；❌ 通常不阻塞
 */
int cgrtos_task_set_deadline(task_id_t id, tick_t deadline)
{
    cgrtos_enter_critical();
    cgrtos_task_t *task = task_find(id);
    if (!task) {
        cgrtos_exit_critical();
        return pdFAIL;
    }
    task->deadline = deadline;
    if (task->state == TASK_READY) {
        cgrtos_sched_remove_ready(task);
        cgrtos_sched_add_ready(task);
    }
    int arm_edf = (task->policy == SCHED_EDF && task->period > 0);
    cgrtos_task_t *arm = arm_edf ? task : 0;
    cgrtos_exit_critical();
    if (arm) {
        cgrtos_sched_edf_arm(arm);
    }
    return pdPASS;
}

/**
 * @brief 查询任务当前实际运行的 CPU 核号
 * @details 返回 TCB 的 run_cpu 字段。
 * @param[in] id 任务 ID
 * @return run_cpu 字段；无效任务返回 0xFF
 * @retval 0..CONFIG_NUM_CORES-1 有效核号
 * @retval 0xFF 无效任务
 * @note 无
 * @warning 无锁快照，SMP 下可能略有滞后
 * @attention ✅ ISR；❌ 不阻塞
 */
uint8_t cgrtos_task_get_run_cpu(task_id_t id)
{
    cgrtos_task_t *task = task_find(id);
    if (!task) {
        return 0xFF;
    }
    return task->run_cpu;
}

/**
 * @brief 按 ID 获取任务 TCB 句柄（内部结构指针）
 * @details 委托 task_find 在 g_tasks 中线性查找匹配 id 的槽位。
 * @param[in] id 任务 ID
 * @return TCB 指针；未找到返回 NULL
 * @retval 非 NULL 找到
 * @retval NULL    未找到
 * @note 应用应视 TCB 为只读句柄，禁止直接改字段
 * @warning 返回指针在任务删除后可能失效（UAF）
 * @attention ✅ 可在 ISR 调用（无锁查找）；❌ 不阻塞
 */
cgrtos_task_t *cgrtos_task_get_handle(task_id_t id)
{
    return task_find(id);
}

/**
 * @brief 查询任务当前对外可见状态
 * @details 查找 TCB 并调用 task_state_to_enum 映射内部 state 到 eTaskState_t。
 * @param[in] id 任务 ID
 * @return eTaskState_t；无效或已删除槽返回 eInvalid
 * @retval eReady/eRunning/eBlocked/eSuspended/eDeleted/eTerminated 对应状态
 * @retval eInvalid 无效 ID
 * @note 无
 * @warning 无锁快照，SMP 下可能略有滞后
 * @attention ✅ ISR；❌ 不阻塞
 */
eTaskState_t cgrtos_task_get_state(task_id_t id)
{
    cgrtos_task_t *task = task_find(id);
    if (!task || task->id == 0) {
        return eInvalid;
    }
    return task_state_to_enum(task->state);
}

/**
 * @brief 计算任务栈高水位（从未使用端起算的剩余空闲栈字节数）
 * @details 自栈底扫描 stack[]：溢出检测模式下跳过金丝雀统计 0xA5 字；否则统计零字；乘以字长返回字节数。
 * @param[in] id 任务 ID
 * @return 剩余未触碰栈空间字节数；无效任务返回 0
 * @retval >0 估测剩余空闲栈
 * @retval 0  无效任务或已几乎用尽
 * @note 需 CONFIG_CHECK_STACK_OVERFLOW 填栈才准确
 * @warning 非精确值，仅调试用
 * @attention ✅ 任务上下文更安全；❌ 不阻塞
 */
uint32_t cgrtos_task_get_stack_high_water_mark(task_id_t id)
{
    cgrtos_task_t *task = task_find(id);
    if (!task) {
        return 0;
    }

    uint32_t words = 0;
#if CONFIG_CHECK_STACK_OVERFLOW
    /* 跳过 stack[0] 金丝雀，统计仍保持 0xA5 模式的字 */
    for (uint32_t i = 1; i < CONFIG_TASK_STACK_WORDS; i++) {
        if (task->stack[i] == 0xA5A5A5A5A5A5A5A5ULL) {
            words++;
        } else {
            break;
        }
    }
#else
    for (uint32_t i = 0; i < CONFIG_TASK_STACK_WORDS; i++) {
        if (task->stack[i] == 0) {
            words++;
        } else {
            break;
        }
    }
#endif
    return words * sizeof(uint64_t);
}

/**
 * @brief 当前任务主动让出 CPU（协作式调度入口）
 * @details 直接调用 cgrtos_sched_yield，由调度器选择下一就绪任务。
 * @return 无
 * @retval 无
 * @note 调度挂起时为空操作
 * @warning 无
 * @attention ❌ ISR（ISR 请用 yield_from_isr）；✅ 引起切换
 */
void cgrtos_task_yield(void)
{
    cgrtos_sched_yield();
}

#if CONFIG_USE_TASK_NOTIFICATIONS
/**
 * @brief 按 eNotifyAction_t 规则将 value 合并到已有通知值
 * @details eSetBits 按位或；eIncrement 加一；eSetValueWithOverwrite 直接替换；
 *          eSetValueWithoutOverwrite 仅在 old 为 0 时写入；未知 action 保持 old_val。
 * @param[in] old_val 合并前的 notify_value
 * @param[in] value   本次通知携带的载荷
 * @param[in] action  合并动作
 * @return 合并后的新 notify_value
 * @retval 任意 uint32_t 合并结果
 * @note 无
 * @warning 无
 * @attention @internal
 */
static uint32_t notify_apply(uint32_t old_val, uint32_t value, eNotifyAction_t action)
{
    /* 1-3. 按 action 合并 notify_value */
    switch (action) {
    case eSetBits:
        return old_val | value;
    case eIncrement:
        return old_val + 1;
    case eSetValueWithOverwrite:
        return value;
    case eSetValueWithoutOverwrite:
        return old_val ? old_val : value;
    default:
        return old_val;
    }
}

/**
 * @brief 向目标任务发送通知（任务上下文，可阻塞调度）
 * @details 按 action 合并 notify_value 并置 notify_pending；若目标 BLOCK_NOTIFY 则 unblock 后 yield。
 * @param[in] task   目标任务 TCB
 * @param[in] value  通知载荷
 * @param[in] action 值更新动作
 * @return 更新前的 notify_value；task 为 NULL 时返回 0
 * @retval 任意 uint32_t 旧值
 * @note 无
 * @warning 与 ISR 通知并发时依赖临界区
 * @attention ❌ ISR 请用 from_isr；✅ 可能唤醒切换
 */
uint32_t cgrtos_task_notify(cgrtos_task_t *task, uint32_t value,
                            eNotifyAction_t action)
{
    if (!task) {
        return 0;
    }

    /* 1. 临界区内更新 notify_value 并置 pending */
    cgrtos_enter_critical();
    uint32_t prev = task->notify_value;
    task->notify_value = notify_apply(prev, value, action);
    task->notify_pending = 1;

    /* 2. 目标在 BLOCK_NOTIFY 上阻塞则 unblock */
    if (task->state == TASK_BLOCKED && task->block_reason == BLOCK_NOTIFY) {
        task->wake_ok = 1;
        cgrtos_sched_unblock(task);
    }
    cgrtos_exit_critical();

    /* 3. yield 使被唤醒任务有机会运行 */
    cgrtos_sched_yield();
    return prev;
}

/**
 * @brief 向目标任务发送通知（ISR 上下文）
 * @details 临界区内更新 notify_value 并置 pending；可 unblock BLOCK_NOTIFY 等待者；
 *          退出后 cgrtos_isr_notify_woken 处理 yield。
 * @param[in]  task   目标任务 TCB
 * @param[in]  value  通知载荷（按 action 解释）
 * @param[in]  action 值更新动作（eSetBits / eSetValueWithOverwrite 等）
 * @param[out] woken  可选；非空则唤醒阻塞在 notify 上的任务时置 pdTRUE
 * @return 更新前的 notify_value；task 为 NULL 时返回 0
 * @retval 任意 uint32_t 旧值
 * @note 须在允许的中断优先级内调用
 * @warning 忽略 woken 且未自动 yield 可能导致延迟调度
 * @attention ✅ ISR；❌ 不阻塞调用 ISR
 */
uint32_t cgrtos_task_notify_from_isr(cgrtos_task_t *task, uint32_t value,
                                     eNotifyAction_t action, BaseType_t *woken)
{
    if (!task) {
        return 0;
    }

    int need_yield = 0;
    cgrtos_enter_critical();
    uint32_t prev = task->notify_value;
    task->notify_value = notify_apply(prev, value, action);
    task->notify_pending = 1;

    if (task->state == TASK_BLOCKED && task->block_reason == BLOCK_NOTIFY) {
        task->wake_ok = 1;
        cgrtos_sched_unblock(task);
        need_yield = 1;
    }
    cgrtos_exit_critical();
    cgrtos_isr_notify_woken(woken, need_yield);
    return prev;
}

/**
 * @brief 当前任务等待通知到达（可选超时）
 * @details 可按掩码在进入/退出时清位；已有 pending 则 fast path；否则 sched_block(BLOCK_NOTIFY) 后 yield。
 * @param[in]  clear_on_entry 进入等待前从 notify_value 清除的位掩码
 * @param[in]  clear_on_exit  成功收到后从 notify_value 清除的位掩码
 * @param[out] value          可选输出指针，接收 notify_value
 * @param[in]  timeout        最大阻塞 tick；0 表示不阻塞、立即返回
 * @return 收到的通知值；超时或无通知返回 0
 * @retval 非0 成功取得通知
 * @retval 0   超时或失败
 * @note 通知值本为 0 时与超时返回 0 可能混淆
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
uint32_t cgrtos_task_notify_wait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                                 uint32_t *value, tick_t timeout)
{
    /* 1. 获取当前核 running 任务 */
    uint8_t cpu = arch_cpu_id();
    cgrtos_task_t *cur = g_current[cpu];
    if (!cur || cur->id == 0) {
        return 0;
    }

    cgrtos_enter_critical();

    if (clear_on_entry) {
        cur->notify_value &= ~clear_on_entry;
    }

    /* 2. 通知已到达则 fast path 直接返回 */
    if (cur->notify_pending) {
        uint32_t out = cur->notify_value;
        if (value) {
            *value = out;
        }
        if (clear_on_exit) {
            cur->notify_value &= ~clear_on_exit;
        }
        cur->notify_pending = 0;
        cgrtos_exit_critical();
        return out;
    }

    /* 3. timeout==0 且无 pending 则非阻塞返回 0 */
    if (timeout == 0) {
        cgrtos_exit_critical();
        return 0;
    }

    /* 4. 阻塞直至 notify 或超时 */
    cgrtos_sched_block(cur, BLOCK_NOTIFY, 0, timeout);
    cgrtos_exit_critical();
    cgrtos_sched_yield();

    /* 5. 唤醒后读取结果并按 clear_on_exit 清除位 */
    cgrtos_enter_critical();
    uint32_t out = cur->notify_pending ? cur->notify_value : 0;
    if (value) {
        *value = out;
    }
    if (clear_on_exit && out) {
        cur->notify_value &= ~clear_on_exit;
    }
    cur->notify_pending = 0;
    cgrtos_exit_critical();
    return out;
}
#endif

/**
 * @brief 忙等直至 mtime 达到绝对截止时刻（亚 tick 精度补齐）
 * @details 循环读取 cgrtos_mtime_read() 直至 mtime >= deadline；纯自旋，不调用 sched_block。
 * @param[in] deadline 目标 mtime 计数（绝对值，非相对增量）
 * @return 无
 * @retval 无
 * @note 用于 delay_us 尾段亚 tick 精确等待
 * @warning 长忙等会占用 CPU
 * @attention @internal
 */
static void delay_busy_until_mtime(uint64_t deadline)
{
    /* 1. 自旋直至 mtime >= deadline */
    while (cgrtos_mtime_read() < deadline) {
        /* 自旋以保持亚 tick 精度 */
    }
}

/**
 * @brief 相对延时若干系统 tick
 * @param ticks 延迟 tick 数；0 等价于仅 yield 一次
 * @details
 * 1. ISR 内或当前无有效任务时直接返回。
 * 2. ticks==0 时仅 sched_yield，不进入延迟队列。
 * 3. 否则临界区内 sched_block(BLOCK_DELAY, timeout=ticks)，yield 后在 wake tick 恢复。
 */
void cgrtos_delay(tick_t ticks)
{
    uint8_t cpu;
    cgrtos_task_t *cur;

    /* 1. ISR 内或无有效任务则直接返回 */
    if (cgrtos_in_isr()) {
        return;
    }

    cpu = arch_cpu_id();
    cur = g_current[cpu];
    if (!cur || cur->id == 0) {
        return;
    }

    /* 2. ticks==0 仅 yield 不进入延迟队列 */
    if (ticks == 0) {
        cgrtos_sched_yield();
        return;
    }

    /* 3. 阻塞 timeout 个 tick 后 yield 等待唤醒 */
    cgrtos_enter_critical();
    cgrtos_sched_block(cur, BLOCK_DELAY, 0, ticks);
    cgrtos_exit_critical();
    cgrtos_sched_yield();
}

/**
 * @brief 阻塞到绝对系统 tick 时刻
 * @param wake 目标唤醒时刻（g_ticks 绝对值）
 * @details
 * 1. ISR 或无有效当前任务时返回。
 * 2. 临界区读取 now=g_ticks；若 wake<=now 说明已过期，立即返回。
 * 3. 否则 sched_block_until(BLOCK_DELAY, wake)，yield 等待 tick 到达 wake。
 */
void cgrtos_delay_until_tick(tick_t wake)
{
    uint8_t cpu;
    cgrtos_task_t *cur;
    tick_t now;

    /* 1. ISR 或无有效当前任务则返回 */
    if (cgrtos_in_isr()) {
        return;
    }

    cpu = arch_cpu_id();
    cur = g_current[cpu];
    if (!cur || cur->id == 0) {
        return;
    }

    cgrtos_enter_critical();
    /* 2. wake 已过期则立即返回 */
    now = g_ticks;
    if (wake <= now) {
        cgrtos_exit_critical();
        return;
    }
    /* 3. 阻塞至绝对 tick wake 后 yield */
    cgrtos_sched_block_until(cur, BLOCK_DELAY, 0, wake);
    cgrtos_exit_critical();
    cgrtos_sched_yield();
}

/**
 * @brief 相对延时若干微秒（tick 粗阻塞 + mtime 忙等混合）
 * @param us 延迟微秒数；0 或 ISR 内无操作
 * @details
 * 1. 将 us 转为 mtime 周期数，计算绝对截止 t_end=mtime_now+cycles。
 * 2. 当剩余时间 >= 2 tick 时，用 cgrtos_delay 粗阻塞（remain/per_tick - 1 tick）。
 * 3. 剩余不足 2 tick 时退出循环，调用 delay_busy_until_mtime 精确补齐。
 * 4. 混合策略兼顾调度友好与亚 tick 精度。
 */
void cgrtos_delay_us(uint32_t us)
{
    uint64_t cycles;
    uint64_t t_end;
    uint64_t per_tick;
    uint64_t remain;

    if (us == 0 || cgrtos_in_isr()) {
        return;
    }

    /* 1. 将 us 转为 mtime 周期并计算绝对截止 t_end */
    cycles = portUS_TO_MTIME(us);
    if (cycles == 0) {
        cycles = 1;
    }

    t_end = cgrtos_mtime_read() + cycles;
    per_tick = portMTIME_PER_TICK;
    if (per_tick == 0) {
        per_tick = 1;
    }

    /* 2. 粗阻塞：剩余 >= 2 tick 时用 tick 延迟，留 headroom 给忙等 */
    for (;;) {
        uint64_t now_m = cgrtos_mtime_read();
        if (now_m >= t_end) {
            return;
        }
        remain = t_end - now_m;
        if (remain < (per_tick * 2ULL)) {
            break;
        }
        {
            tick_t coarse = (tick_t)((remain / per_tick) - 1ULL);
            if (coarse == 0) {
                break;
            }
            cgrtos_delay(coarse);
        }
    }

    /* 3. 尾段 mtime 忙等精确到微秒 */
    delay_busy_until_mtime(t_end);
}

/**
 * @brief 相对延时若干毫秒
 * @param ms 延迟毫秒数；0 等价于 delay(0)
 * @details
 * 1. ms==0 时转调 cgrtos_delay(0) 仅 yield。
 * 2. ms 过大导致 us 乘法溢出时，退化为纯 tick 路径 portMS_TO_TICK。
 * 3. 常规模围内转 us（ms*1000）委托 cgrtos_delay_us 实现。
 */
void cgrtos_delay_ms(uint32_t ms)
{
    /* 1. ms==0 仅 yield */
    if (ms == 0) {
        cgrtos_delay(0);
        return;
    }
#if !CONFIG_DELAY_BUSY_US
    /* 板级关闭亚 tick 忙等：纯 tick 延时（如 AArch64 QEMU） */
    cgrtos_delay(portMS_TO_TICK(ms));
    return;
#else
    /* 2. 乘法溢出则退化为纯 tick 路径 */
    if (ms > (0xFFFFFFFFU / 1000U)) {
        cgrtos_delay(portMS_TO_TICK(ms));
        return;
    }
    /* 3. 常规模围转 us 委托 delay_us */
    cgrtos_delay_us(ms * 1000U);
#endif
}

/**
 * @brief 绝对周期延时（FreeRTOS vTaskDelayUntil 语义）
 * @param prev_wake 上次唤醒 tick（入参/出参，由本函数推进）
 * @param increment 周期间隔 tick 数
 * @details
 * 1. prev_wake 或 increment 无效、或 ISR 内则返回。
 * 2. 计算 next=*prev_wake+increment，并写回 *prev_wake=next。
 * 3. 读取 now=g_ticks；若 next>now 则 delay_until_tick(next)。
 * 4. 若已错过 deadline（next<=now），立即返回不拉伸周期（不补偿漂移）。
 */
void cgrtos_delay_until(tick_t *prev_wake, tick_t increment)
{
    tick_t now;
    tick_t next;

    /* 1. 参数无效或 ISR 内则返回 */
    if (!prev_wake || increment == 0 || cgrtos_in_isr()) {
        return;
    }

    /* 2. 计算 next 并写回 prev_wake */
    next = *prev_wake + increment;
    *prev_wake = next;
    /* 3. next>now 则阻塞至 next，否则立即返回不补偿漂移 */
    now = cgrtos_get_ticks();
    if (next > now) {
        cgrtos_delay_until_tick(next);
    }
    /* 已错过 deadline：立即返回，不延长周期 */
}

/**
 * @brief 各 CPU 核 idle 任务入口（最低优先级后台循环）
 * @param arg 未使用（核号通过 arch_cpu_id() 获取）
 * @details
 * 1. 无限循环：可选 idle 钩子 → sched_idle_steal 工作窃取。
 * 2. QEMU busy 泵模式：hart0 自旋推进 mtime；hart1 短自旋（禁止 WFI 以免时间基冻结）。
 * 3. 非 busy 模式执行 WFI 等待中断。
 * 4. 若 g_yield_pending 置位则 yield，处理跨核唤醒的待切换请求。
 */
void cgrtos_idle_task_entry(void *arg)
{
    (void)arg;
    uint8_t cpu = arch_cpu_id();

#if CONFIG_IDLE_BUSY_PUMP
    const uint64_t tpi = CONFIG_TIMER_CLOCK_HZ / CONFIG_TICK_RATE_HZ;
#endif

    while (1) {
#if CONFIG_USE_HOOKS
        if (g_idle_hook) {
            g_idle_hook();
        }
#if CONFIG_IDLE_SLEEP_HOOK
        if (g_idle_sleep_hook) {
            g_idle_sleep_hook();
        }
#endif
#endif
        /* 1. 睡眠/忙等前先尝试从过载核窃取就绪任务 */
        cgrtos_sched_idle_steal();

#if CONFIG_IDLE_BUSY_PUMP
        if (cpu == 0) {
            /*
             * 2. QEMU hart0：自旋推进 mtime 使 SysTimer IRQ 持续触发。
             * 禁止在此软调用 cgrtos_tick_handler，会与硬 MTIP 路径重复计数。
             */
            uint64_t t0 = cgrtos_mtime_read();
            while ((cgrtos_mtime_read() - t0) < tpi) {
            }
        } else {
            /* 2. QEMU hart1 短自旋（禁止 WFI 以免时间基冻结） */
            for (volatile int i = 0; i < 64; i++) {
            }
        }
#else
        /* 2. 非 busy 模式：低功耗等待中断（真实硅片入口） */
        arch_cpu_wait();
#endif

        /* 3. 跨核唤醒 pending 则 yield 处理切换 */
        if (g_yield_pending[cpu]) {
            cgrtos_sched_yield();
        }
    }
}

/**
 * @brief 初始化各 CPU 核的 idle 任务 TCB 与初始栈帧
 * @details
 * 1. 对每个核 i 清零 g_idle[i]，初始化调度链表节点。
 * 2. 设置名 "idle_i"、prio=0、state=READY、policy=RR、cpu_aff=run_cpu=i。
 * 3. task_init_stack 绑定 cgrtos_idle_task_entry，将 g_current[i] 指向 idle TCB。
 * 4. idle 任务 id 保持 0，表示非用户创建任务。
 */
void cgrtos_init_idle_tasks(void)
{
    for (int i = 0; i < CONFIG_NUM_CORES; i++) {
        /* 1. 清零 idle TCB 并初始化调度链表节点 */
        memset(&g_idle[i], 0, sizeof(g_idle[i]));
        list_init_item(&g_idle[i].delayed_item);
        list_init_item(&g_idle[i].cfs_item);
        list_init_item(&g_idle[i].edf_item);
        list_init_item(&g_idle[i].edf_rel_item);
        g_idle[i].id = 0;
        /* 2. 设置名称、prio=0、READY、RR 策略与核亲和性 */
        cgrtos_snprintf(g_idle[i].name, CGRTOS_TASK_NAME_MAX, "idle_%d", i);
        g_idle[i].prio = 0;
        g_idle[i].base_prio = 0;
#if CONFIG_USE_PREEMPT_THRESH
        g_idle[i].preempt_thresh = 0;
#endif
        g_idle[i].state = TASK_READY;
        g_idle[i].policy = SCHED_RR;
        g_idle[i].cpu_aff = (uint8_t)i;
        g_idle[i].run_cpu = (uint8_t)i;
        g_idle[i].slice_remain = CONFIG_TIME_SLICE_TICKS;
        /* 3. 构建栈帧并将 g_current[i] 指向 idle TCB */
        g_idle[i].sp = task_init_stack(&g_idle[i], cgrtos_idle_task_entry,
                                       (void *)(uintptr_t)i);
        g_current[i] = &g_idle[i];
    }
}

/**
 * @brief 导出任务列表到缓冲区
 * @details 临界区内扫描 g_tasks，跳过 id==0 或 DELETED 槽；填充 out[n] 并统计总数。
 * @param[out] out 输出数组；NULL 时仅计数
 * @param[in]  max  out 容量（条数）
 * @return 存活任务总数；out 非 NULL 时最多写入 max 条
 * @retval >=0 任务总数
 * @note out 为 NULL 时返回当前任务数
 * @warning 无
 * @attention ❌ ISR（持 g_klock）；❌ 不阻塞
 */
uint32_t cgrtos_task_list_export(cgrtos_task_info_t *out, uint32_t max)
{
    uint32_t n = 0;
    uint32_t i;

    cgrtos_enter_critical();
    for (i = 0; i < CONFIG_MAX_TASKS; i++) {
        cgrtos_task_t *t = &g_tasks[i];
        if (t->id == 0 || t->state == TASK_DELETED) {
            continue;
        }
        if (out && n < max) {
            out[n].id = t->id;
            strncpy(out[n].name, t->name, CGRTOS_TASK_NAME_MAX - 1);
            out[n].name[CGRTOS_TASK_NAME_MAX - 1] = 0;
            out[n].prio = t->prio;
            out[n].state = t->state;
            out[n].policy = t->policy;
            out[n].exec_ticks = t->exec;
            out[n].stack_hwm = cgrtos_task_get_stack_high_water_mark(t->id);
        }
        n++;
    }
    cgrtos_exit_critical();
    (void)max;
    return n;
}
