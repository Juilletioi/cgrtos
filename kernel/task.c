/**
 * @file task.c
 * @brief 任务生命周期、通知、延迟与 idle
 * @details 提供任务创建/删除/挂起/恢复、优先级与亲和性、
 *          周期/截止时间、通知、延迟及 per-core idle 任务实现。
 *
 * 任务栈内嵌于 TCB（CONFIG_TASK_STACK_WORDS）。
 * 亲和性：cpu_aff==0xFF 表示可迁移；否则固定到指定 hart。
 * Idle：可选 busy mtime 泵（QEMU）；执行工作窃取。
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

extern char __global_pointer$;

#if CONFIG_USE_HOOKS
extern cgrtos_hook_fn_t g_idle_hook;
#endif

/**
 * @brief 初始化任务栈帧，使其可从 context_switch 首次启动
 * @param task 目标任务 TCB，其内嵌 stack[] 将被填充
 * @param fn   任务入口函数，写入 trap 帧 mepc
 * @param arg  传递给入口的 a0 参数
 * @return 初始栈指针，指向 trap 帧底部（34 字寄存器 + 对齐区）
 * @details
 * 1. 将栈顶向下预留 16 字节对齐区，再向下分配 34 个 uint64_t 的 trap 帧。
 * 2. 若启用栈溢出检测，用 0xA5 填充整栈并在 stack[0] 写入金丝雀；否则仅清零 trap 帧。
 * 3. 按 trap_vector / context_switch 布局写入 gp、a0、mcause、mepc、mstatus、mscratch。
 * 4. mstatus 设为 M 模式、MPIE=1、MIE=0，使 mret 进入任务时中断仍关闭直至调度器开启。
 * 5. 返回 sp 供 TCB->sp 保存，供 start_first_task / context_switch 使用。
 */
static uint64_t *task_init_stack(cgrtos_task_t *task, task_func_t fn, void *arg)
{
    /* 1. 栈顶 16 字节对齐后向下分配 trap 帧 */
    uint64_t *sp = (uint64_t *)((uint8_t *)task->stack +
                                sizeof(task->stack) - 16);
    sp -= 34;

#if CONFIG_CHECK_STACK_OVERFLOW
    /* 2a. 整栈填充模式字并写入栈底金丝雀 */
    memset(task->stack, 0xA5, sizeof(task->stack));
    task->stack[0] = STACK_CANARY;
#else
    /* 2b. 仅清零即将使用的 trap 帧区域 */
    memset(sp, 0, 34 * sizeof(uint64_t));
#endif

    /* 3. 按 trap 帧布局填充关键寄存器（与 trap_vector 约定一致） */
    sp[1]  = (uint64_t)(uintptr_t)&__global_pointer$; /* gp — medany 代码段必需 */
    sp[8]  = (uint64_t)arg;    /* a0：任务入口参数 */
    sp[30] = 0;                /* mcause（首次启动未使用） */
    sp[31] = (uint64_t)fn;     /* mepc：mret 跳转至任务入口 */
    sp[32] = 0x1880;           /* mstatus: MPP=M, MPIE=1, MIE=0 */
    sp[33] = 0;                /* mscratch */
    return sp;
}

/**
 * @brief 在 g_tasks 数组中分配可复用的空闲 TCB 槽位
 * @return 可用 TCB 指针；任务表满或所有候选槽仍被某核 g_current 引用时返回 NULL
 * @details
 * 1. 线性扫描 g_tasks[0..CONFIG_MAX_TASKS)，寻找 id==0 或 state==TASK_DELETED 的槽。
 * 2. 对每个候选槽，遍历所有 hart 的 g_current[]，确认无核仍持有该 TCB 指针。
 * 3. 若某核 g_current 仍指向该 TCB，跳过以免删除与切换竞态导致 use-after-free。
 * 4. 返回第一个通过存活检查的 TCB 指针；扫描完毕无可用槽则返回 NULL。
 */
static cgrtos_task_t *task_alloc_slot(void)
{
    for (uint32_t i = 0; i < CONFIG_MAX_TASKS; i++) {
        cgrtos_task_t *t = &g_tasks[i];
        /* 1. 跳过仍被占用的有效任务槽 */
        if (t->id != 0 && t->state != TASK_DELETED) {
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
 * @param id 创建时分配的任务 ID（g_next_id 序列）
 * @return 匹配的 TCB 指针；未找到返回 NULL
 * @details
 * 1. 线性扫描 g_tasks 全表。
 * 2. 比较 task->id 与目标 id，相等则返回该 TCB 地址。
 * 3. 扫描结束无匹配则返回 NULL。
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
 * @param state 内部任务状态（TASK_READY / RUNNING / …）
 * @return 对应的 eTaskState_t；未知状态返回 eInvalid
 * @details
 * 1. 按 switch-case 逐一映射 READY/RUNNING/BLOCKED/SUSPENDED/DELETED。
 * 2. 未覆盖的枚举值走 default 分支返回 eInvalid。
 */
static eTaskState_t task_state_to_enum(task_state_t state)
{
    switch (state) {
    case TASK_READY:    return eReady;
    case TASK_RUNNING:  return eRunning;
    case TASK_BLOCKED:  return eBlocked;
    case TASK_SUSPENDED: return eSuspended;
    case TASK_DELETED:  return eDeleted;
    default:            return eInvalid;
    }
}

/**
 * @brief 创建新任务并加入就绪队列
 * @param name   任务名（可为 NULL，默认 "task"）
 * @param fn     入口函数，不可为 NULL
 * @param arg    传递给入口的 void* 参数
 * @param prio   优先级；超过 CONFIG_MAX_PRIORITY 则截断至最大值
 * @param policy 调度策略（RR/PRIORITY/CFS/EDF/HYBRID）
 * @return 新任务 ID；参数无效、任务表满或槽位分配失败时返回 (task_id_t)-1
 * @details
 * 1. 校验 fn 非空，并将 prio 截断到 CONFIG_MAX_PRIORITY。
 * 2. 进入临界区，检查 g_task_count 是否已达 CONFIG_MAX_TASKS 上限。
 * 3. 调用 task_alloc_slot 获取可复用 TCB，失败则退出临界区并返回 -1。
 * 4. 清零 TCB，初始化各调度链表节点（delayed/cfs/edf），分配单调递增 id。
 * 5. 设置名称、优先级、策略、亲和性（默认 0xFF 可迁移）及初始 run_cpu。
 * 6. 调用 task_init_stack 构建 trap 帧，记录 last_run，加入就绪队列。
 * 7. 退出临界区；SMP 下若任务落在对端核则发 IPI 唤醒调度。
 * 8. 非临时任务名 "tmp" 时打印创建日志，返回新 id。
 */
task_id_t cgrtos_task_create(const char *name, task_func_t fn, void *arg,
                             uint8_t prio, sched_policy_t policy)
{
    /* 1. 校验 fn 非空并截断 prio */
    if (!fn) {
        return (task_id_t)-1;
    }
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
    task->policy = policy;
    task->cpu_aff = 0xFF;
#if CONFIG_SMP_INITIAL_PLACE
    task->run_cpu = cgrtos_sched_least_loaded_core();
#else
    task->run_cpu = (uint8_t)read_csr(mhartid);
#endif
    task->slice_remain = CONFIG_TIME_SLICE_TICKS;
    /* 6. 构建 trap 帧并加入就绪队列 */
    task->sp = task_init_stack(task, fn, arg);
    task->last_run = g_ticks;

    cgrtos_sched_add_ready(task);
    g_task_create_count++;
    cgrtos_exit_critical();

    /* 7. 对端核需 IPI 才能调度新就绪任务 */
#if CONFIG_NUM_CORES > 1
    if (g_secondary_online &&
        task->run_cpu != (uint8_t)read_csr(mhartid)) {
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
 * @param id 待删除任务 ID
 * @return pdPASS 成功；任务不存在 / 试图删 idle 返回 pdFAIL
 * @details
 * 1. 进入临界区，按 id 查找 TCB；无效则 pdFAIL。
 * 2. 禁止删除各核 idle TCB（比较指针）。
 * 3. 调用 cgrtos_mutex_force_release_owned 释放其持有的全部互斥量。
 * 4. READY → 摘就绪队列；BLOCKED → purge_waits。
 * 5. 清除 EDF 周期并解除时间轮。
 * 6. 置 state=TASK_DELETED；g_task_count--；g_task_delete_count++。
 * 7. **不立即清 id**：若仍为某核 g_current，保留 id 防槽位复用；
 *    仅当已不在任何核运行时才 id=0 立刻回收。
 * 8. 远程核仍跑该 TCB 则发 IPI；exit_critical 后 yield。
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

    uint8_t self = (uint8_t)read_csr(mhartid);
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

    /* yield：自删或同核 RUNNING 时尽快切走 */
    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 切换离开已删除任务后回收 TCB（清 id）
 * @param task 可能为 TASK_DELETED 的 TCB
 * @details
 * 1. task 为空或 state 非 DELETED 则返回。
 * 2. 若仍被任意核 g_current 引用则暂不回收。
 * 3. 否则将 id 置 0，允许 task_alloc_slot 复用。
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
 * @param id 任务 ID
 * @return pdPASS 完好；pdFAIL 溢出/无效
 * @details
 * 1. 查找 TCB；失败返回 pdFAIL。
 * 2. CONFIG_CHECK_STACK_OVERFLOW 关闭时直接 pdPASS。
 * 3. 比较 stack[0] 与 STACK_CANARY_VALUE。
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
 * @param task 溢出任务（可空）
 * @details
 * 1. 递增 g_stack_overflow_count（定义于 cgrtos.c）。
 * 2. 若注册了栈溢出钩子则调用。
 * 3. 打印任务名并 cgrtos_assert_failed 停机。
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
 * @param id 任务 ID
 * @return exec；无效任务返回 0
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
 * @param id 任务 ID
 * @return pdPASS 成功；任务不存在返回 pdFAIL
 * @details
 * 1. 进入临界区，查找 TCB；未找到则返回 pdFAIL。
 * 2. 若 READY，从就绪队列移除；若 BLOCKED，清除等待关系。
 * 3. 设置 state=TASK_SUSPENDED，退出临界区。
 * 4. 调用 sched_yield 触发可能的调度切换。
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
 * @param id 任务 ID
 * @return pdPASS 成功；任务不存在或当前非 SUSPENDED 返回 pdFAIL
 * @details
 * 1. 进入临界区，查找 TCB；不存在或 state 非 SUSPENDED 则失败。
 * 2. 调用 sched_add_ready 重新加入就绪队列。
 * 3. 退出临界区并 yield，使高优先级就绪任务有机会运行。
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
 * @param id   任务 ID
 * @param prio 新优先级（0..CONFIG_MAX_PRIORITY）
 * @return pdPASS 成功；prio 越界或任务不存在返回 pdFAIL
 * @details
 * 1. 校验 prio 不超过 CONFIG_MAX_PRIORITY。
 * 2. 进入临界区查找 TCB；未找到则失败。
 * 3. 若任务当前在 READY 队列，先 remove 再改 prio/base_prio，再 add_ready（保持队列有序）。
 * 4. 非 READY 状态仅更新 prio/base_prio 字段。
 * 5. 退出临界区并 yield。
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

    if (task->state == TASK_READY) {
        cgrtos_sched_add_ready(task);
    }
    cgrtos_exit_critical();

    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 设置任务 CPU 亲和性（硬绑定或可迁移）
 * @param id  任务 ID
 * @param cpu 目标核编号；0xFF 表示无亲和性限制、可负载均衡迁移
 * @return pdPASS 成功；cpu 非法、次核离线或任务不存在返回 pdFAIL
 * @details
 * 1. 校验 cpu：须为 0xFF 或有效核号；硬绑定到离线次核则拒绝。
 * 2. 进入临界区查找 TCB；READY 时先从就绪队列移除。
 * 3. 写入 cpu_aff；若仍 READY 则按新亲和性重新入队。
 * 4. 若绑定到非本核，向目标核发 IPI；最后 yield。
 */
int cgrtos_task_set_affinity(task_id_t id, uint8_t cpu)
{
    if (cpu != 0xFF && cpu >= CONFIG_NUM_CORES) {
        return pdFAIL;
    }
    /* 次核尚未 online 时不允许硬亲和到 hart1 */
    if (cpu != 0xFF && cpu != 0 && !g_secondary_online) {
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

    if (cpu != 0xFF && cpu != (uint8_t)read_csr(mhartid)) {
        cgrtos_smp_send_ipi(cpu);
    }
    cgrtos_sched_yield();
    return pdPASS;
}

/**
 * @brief 设置 EDF 任务的周期（tick）
 * @param id     任务 ID
 * @param period 周期 tick 数；0 表示清除周期
 * @return pdPASS 成功；任务不存在返回 pdFAIL
 * @details
 * 1. 进入临界区查找 TCB；失败则返回 pdFAIL。
 * 2. 写入 task->period。
 * 3. 若策略为 SCHED_EDF 且 period>0，设置 deadline=g_ticks+period 并挂载 EDF 时间轮。
 * 4. 退出临界区并返回 pdPASS。
 */
int cgrtos_task_set_period(task_id_t id, tick_t period)
{
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
}

/**
 * @brief 设置任务的绝对截止时间（tick）
 * @param id       任务 ID
 * @param deadline 绝对 deadline（相对 g_ticks 的时间点）
 * @return pdPASS 成功；任务不存在返回 pdFAIL
 * @details
 * 1. 进入临界区查找 TCB；失败则返回 pdFAIL。
 * 2. 更新 task->deadline。
 * 3. 若任务 READY，从就绪队列移除并按新 deadline 重新入队（EDF 排序）。
 * 4. 若策略为 EDF 且 period>0，退出临界区后调用 edf_arm 同步时间轮。
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
 * @param id 任务 ID
 * @return run_cpu 字段；无效任务返回 0xFF
 * @details
 * 1. 调用 task_find 定位 TCB。
 * 2. 未找到则返回 0xFF；否则返回 task->run_cpu。
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
 * @param id 任务 ID
 * @return TCB 指针；未找到返回 NULL
 * @details
 * 1. 委托 task_find 在 g_tasks 中线性查找匹配 id 的槽位。
 * 2. 直接返回查找结果，供 IPC/同步原语等内核模块引用任务对象。
 */
cgrtos_task_t *cgrtos_task_get_handle(task_id_t id)
{
    return task_find(id);
}

/**
 * @brief 查询任务当前对外可见状态
 * @param id 任务 ID
 * @return eTaskState_t；无效或已删除槽返回 eInvalid
 * @details
 * 1. 查找 TCB；不存在或 id==0（空槽）则返回 eInvalid。
 * 2. 调用 task_state_to_enum 将内部 state 转为 API 枚举并返回。
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
 * @param id 任务 ID
 * @return 剩余未触碰栈空间字节数；无效任务返回 0
 * @details
 * 1. 查找 TCB；失败返回 0。
 * 2. 自栈底（低地址）向上扫描 stack[]：溢出检测模式下跳过金丝雀，统计连续 0xA5 填充字。
 * 3. 非检测模式下统计连续零字；遇到首个被改写字即停止。
 * 4. 将累计字数乘以 sizeof(uint64_t) 返回字节数。
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
 * @details
 * 1. 直接调用 cgrtos_sched_yield，由调度器选择下一就绪任务。
 * 2. 若当前无更高或同优先级就绪任务，可能立即返回继续运行。
 */
void cgrtos_task_yield(void)
{
    cgrtos_sched_yield();
}

#if CONFIG_USE_TASK_NOTIFICATIONS
/**
 * @brief 按 eNotifyAction_t 规则将 value 合并到已有通知值
 * @param old_val 合并前的 notify_value
 * @param value   本次通知携带的载荷
 * @param action  合并动作（置位/递增/覆盖/无覆盖写入）
 * @return 合并后的新 notify_value
 * @details
 * 1. 根据 action 分支：eSetBits 按位或；eIncrement 加一。
 * 2. eSetValueWithOverwrite 直接替换；eSetValueWithoutOverwrite 仅在 old 为 0 时写入。
 * 3. 未知 action 保持 old_val 不变。
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
 * @param task   目标任务 TCB
 * @param value  通知载荷
 * @param action 值更新动作
 * @return 更新前的 notify_value；task 为 NULL 时返回 0
 * @details
 * 1. 进入临界区，保存 prev=notify_value，按 action 写入新值并置 notify_pending。
 * 2. 若目标因 BLOCK_NOTIFY 阻塞，置 wake_ok 并 sched_unblock。
 * 3. 退出临界区后 yield，使被唤醒任务有机会运行；返回 prev。
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
 * @param task   目标任务 TCB
 * @param value  通知载荷
 * @param action 值更新动作
 * @return 更新前的 notify_value；task 为 NULL 时返回 0
 * @details
 * 1. 与 task 版本相同：临界区内更新 notify_value 与 notify_pending。
 * 2. 若目标在 BLOCK_NOTIFY 上阻塞，unblock 后退出临界区并 yield_from_isr。
 * 3. 未唤醒任务则仅退出临界区；返回 prev。
 */
uint32_t cgrtos_task_notify_from_isr(cgrtos_task_t *task, uint32_t value,
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

    /* 2. 目标在 BLOCK_NOTIFY 上阻塞则 unblock 并 yield_from_isr */
    if (task->state == TASK_BLOCKED && task->block_reason == BLOCK_NOTIFY) {
        task->wake_ok = 1;
        cgrtos_sched_unblock(task);
        cgrtos_exit_critical();
        cgrtos_sched_yield_from_isr();
        return prev;
    }
    cgrtos_exit_critical();
    return prev;
}

/**
 * @brief 当前任务等待通知到达（可选超时）
 * @param clear_on_entry 进入等待前从 notify_value 清除的位掩码
 * @param clear_on_exit  成功收到后从 notify_value 清除的位掩码
 * @param value          可选输出指针，接收 notify_value
 * @param timeout        最大阻塞 tick；0 表示不阻塞、立即返回
 * @return 收到的通知值；超时或无通知返回 0
 * @details
 * 1. 获取当前核 g_current；无效则返回 0。
 * 2. 临界区内按 clear_on_entry 清除指定位；若已有 pending 通知则立即取走并返回。
 * 3. timeout==0 且无 pending 则非阻塞返回 0。
 * 4. 否则 sched_block(BLOCK_NOTIFY)，yield 后再次进入临界区读取结果。
 * 5. 按 clear_on_exit 清除位、清零 notify_pending，返回通知值。
 */
uint32_t cgrtos_task_notify_wait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                                 uint32_t *value, tick_t timeout)
{
    /* 1. 获取当前核 running 任务 */
    uint8_t cpu = (uint8_t)read_csr(mhartid);
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
 * @param deadline 目标 mtime 计数（绝对值，非相对增量）
 * @details
 * 1. 循环读取 cgrtos_mtime_read()，直至当前 mtime >= deadline。
 * 2. 纯自旋，不调用 sched_block，用于 delay_us 尾段亚 tick 精确等待。
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

    cpu = (uint8_t)read_csr(mhartid);
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

    cpu = (uint8_t)read_csr(mhartid);
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
    /* 2. 乘法溢出则退化为纯 tick 路径 */
    if (ms > (0xFFFFFFFFU / 1000U)) {
        cgrtos_delay(portMS_TO_TICK(ms));
        return;
    }
    /* 3. 常规模围转 us 委托 delay_us */
    cgrtos_delay_us(ms * 1000U);
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
 * @param arg 未使用（核号通过 read_csr(mhartid) 获取）
 * @details
 * 1. 无限循环：可选 idle 钩子 → sched_idle_steal 工作窃取。
 * 2. QEMU busy 泵模式：hart0 自旋推进 mtime；hart1 短自旋（禁止 WFI 以免时间基冻结）。
 * 3. 非 busy 模式执行 WFI 等待中断。
 * 4. 若 g_yield_pending 置位则 yield，处理跨核唤醒的待切换请求。
 */
void cgrtos_idle_task_entry(void *arg)
{
    (void)arg;
    uint8_t cpu = (uint8_t)read_csr(mhartid);

#if CONFIG_IDLE_BUSY_PUMP
    const uint64_t tpi = CONFIG_TIMER_CLOCK_HZ / CONFIG_TICK_RATE_HZ;
#endif

    while (1) {
#if CONFIG_USE_HOOKS
        if (g_idle_hook) {
            g_idle_hook();
        }
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
        /* 2. 非 busy 模式执行 WFI 等待中断 */
        asm volatile("wfi" ::: "memory");
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
