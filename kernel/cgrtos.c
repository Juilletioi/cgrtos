/**
 * @file cgrtos.c
 * @brief 内核全局变量、临界区与 SMP 启动
 * @details 提供 CG-RTOS 内核全局状态、可嵌套临界区、自旋锁、
 *          系统初始化/启动、次核启动、运行时统计及 ISR 进出跟踪。
 *
 * 启动流程（另见 startup.S）：
 *   hart0: cgrtos_init → 唤醒次核 MSIP → 应用创建任务 → cgrtos_start
 *   hart1: secondary_main → 等待 g_sched_run → idle
 *
 * 临界区：每核可嵌套的 IRQ 关闭 + 全局自旋锁 g_klock。
 */
#include "cgrtos.h"
#include <string.h>

/** @brief 全局任务控制块数组 */
cgrtos_task_t g_tasks[CONFIG_MAX_TASKS];
/** @brief 当前存活任务数量 */
uint32_t      g_task_count;
/** @brief 各核当前运行任务指针 */
cgrtos_task_t *g_current[CONFIG_NUM_CORES];
/** @brief 各核 idle 任务 TCB */
cgrtos_task_t g_idle[CONFIG_NUM_CORES];
/** @brief 系统 tick 计数（原子访问） */
tick_t        g_ticks;
/** @brief 全局上下文切换总次数 */
uint32_t      g_cs_count;
/** @brief 各核上下文切换次数 */
uint32_t      g_cs_count_core[CONFIG_NUM_CORES];
/** @brief 负载均衡迁移计数 */
uint32_t      g_lb_migrate_count;
/** @brief 工作窃取计数 */
uint32_t      g_lb_steal_count;
/** @brief 调度器已启动标志（次核轮询） */
uint8_t       g_sched_run;
/** @brief 内核全局自旋锁 */
spinlock_t    g_klock;
/** @brief 各核待处理 yield 标志 */
volatile uint8_t g_yield_pending[CONFIG_NUM_CORES];
/** @brief hart0 置位后经 IPI 通知次核执行本地 tick */
volatile uint8_t g_remote_tick[CONFIG_NUM_CORES];

/** @brief 次核启动同步魔数（须在 .data，避免热启动残留 BSS 魔数竞态） */
volatile uint32_t g_boot_sync __attribute__((section(".data"))) = 0;
/** @brief 次核已进入调度器标志 */
volatile uint32_t g_secondary_online;
/** @brief hart1 启动阶段调试面包屑（.data，避免 BSS 清零抹掉） */
volatile uint32_t g_hart1_stage __attribute__((section(".data"))) = 0;

#if CONFIG_USE_HOOKS
/** @brief idle 任务钩子函数指针 */
cgrtos_hook_fn_t g_idle_hook;
/** @brief tick 钩子函数指针 */
cgrtos_hook_fn_t g_tick_hook;
/** @brief malloc 失败钩子函数指针 */
cgrtos_malloc_failed_hook_t g_malloc_failed_hook;

/**
 * @brief 注册 idle 任务钩子函数
 * @param hook 钩子回调；NULL 表示清除
 * @details
 * 1. 将全局 g_idle_hook 设为 hook。
 * 2. idle 任务每轮循环开头若 hook 非空则调用。
 */
void cgrtos_set_idle_hook(cgrtos_hook_fn_t hook)
{
    g_idle_hook = hook;
}

/**
 * @brief 注册系统 tick 钩子函数
 * @param hook 钩子回调；NULL 表示清除
 * @details
 * 1. 将全局 g_tick_hook 设为 hook。
 * 2. 由 tick ISR 路径在适当时机调用（若已注册）。
 */
void cgrtos_set_tick_hook(cgrtos_hook_fn_t hook)
{
    g_tick_hook = hook;
}

/**
 * @brief 注册堆分配失败钩子
 * @param hook 钩子回调；NULL 表示清除
 * @details
 * 1. 将全局 g_malloc_failed_hook 设为 hook。
 */
void cgrtos_set_malloc_failed_hook(cgrtos_malloc_failed_hook_t hook)
{
    g_malloc_failed_hook = hook;
}
#endif /* CONFIG_USE_HOOKS */

/** @brief 断言失败计数 */
uint32_t g_assert_count;
/** @brief 栈溢出检测计数 */
uint32_t g_stack_overflow_count;
/** @brief 断言钩子（halt 前） */
cgrtos_assert_hook_t g_assert_hook;
/** @brief 栈溢出钩子 */
cgrtos_stack_overflow_hook_t g_stack_overflow_hook;

/**
 * @brief 注册断言失败钩子
 * @param hook 回调；NULL 清除。仍会在钩子返回后 halt
 * @details
 * 1. 保存 g_assert_hook。
 */
void cgrtos_set_assert_hook(cgrtos_assert_hook_t hook)
{
    g_assert_hook = hook;
}

/**
 * @brief 注册栈溢出钩子
 * @param hook 回调；NULL 清除。默认随后仍 assert halt
 * @details
 * 1. 保存 g_stack_overflow_hook。
 */
void cgrtos_set_stack_overflow_hook(cgrtos_stack_overflow_hook_t hook)
{
    g_stack_overflow_hook = hook;
}

/** @brief 各核临界区嵌套深度 */
static uint32_t g_crit_nest[CONFIG_NUM_CORES];
/** @brief 各核进入临界区时保存的 mstatus */
static uint64_t g_crit_saved[CONFIG_NUM_CORES];
/** @brief 各核 ISR 嵌套深度（不可用全局计数，否则跨核误判） */
static volatile uint32_t g_in_isr[CONFIG_NUM_CORES];

extern uint32_t g_sem_cnt;
extern uint32_t g_mtx_cnt;
extern uint32_t g_q_cnt;
extern uint32_t g_task_create_count;
extern uint32_t g_task_delete_count;

void cgrtos_sched_init(void);

/**
 * @brief 获取自旋锁（Test-And-Set 忙等）
 * @param lock 自旋锁变量指针
 * @details
 * 1. 循环 __sync_lock_test_and_set(lock, 1) 直至成功获取（返回 0）。
 * 2. __sync_synchronize 内存屏障，保证临界区前的写对该锁持有者可见。
 */
void cgrtos_spin_lock(spinlock_t *lock)
{
    while (__sync_lock_test_and_set(lock, 1)) {
    }
    __sync_synchronize();
}

/**
 * @brief 释放自旋锁
 * @param lock 自旋锁变量指针
 * @details
 * 1. __sync_synchronize 屏障，保证临界区内写先于锁释放完成。
 * 2. __sync_lock_release(lock) 原子写 0 释放锁。
 */
void cgrtos_spin_unlock(spinlock_t *lock)
{
    __sync_synchronize();
    __sync_lock_release(lock);
}

/**
 * @brief 保存 mstatus 并关闭全局 M 模式中断（MIE）
 * @return 进入前的完整 mstatus 值，供 cgrtos_irq_restore 恢复
 * @details
 * 1. read_csr(mstatus) 保存当前状态。
 * 2. clear_csr_bits 清除 MIE 位（0x8），禁止 M 模式中断。
 * 3. 返回 flags 供配对 restore 使用。
 */
uint64_t cgrtos_irq_save(void)
{
    uint64_t flags = read_csr(mstatus);
    clear_csr_bits(mstatus, 0x8); /* 清除 MIE */
    return flags;
}

/**
 * @brief 恢复 mstatus（含 MIE 等全部位）
 * @param flags cgrtos_irq_save 返回的 mstatus 快照
 * @details
 * 1. write_csr(mstatus, flags) 一次性恢复进入 irq_save 前的中断使能状态。
 */
void cgrtos_irq_restore(uint64_t flags)
{
    write_csr(mstatus, flags);
}

/**
 * @brief 进入内核临界区（可嵌套；首层关中断并持全局锁）
 * @details
 * 1. 读取当前 hartid 作为 cpu 索引。
 * 2. 若 g_crit_nest[cpu]==0（最外层），irq_save 关中断并 spin_lock g_klock。
 * 3. g_crit_nest[cpu]++ 记录嵌套深度。
 * @note 必须与 cgrtos_exit_critical 配对调用
 */
void cgrtos_enter_critical(void)
{
    /* 1. 读取当前 hartid 作为 cpu 索引 */
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    /* 2. 若 g_crit_nest[cpu]==0（最外层），irq_save 关中断并 spin_lock g_klock */
    if (g_crit_nest[cpu] == 0) {
        g_crit_saved[cpu] = cgrtos_irq_save();
        cgrtos_spin_lock(&g_klock);
    }
    /* 3. g_crit_nest[cpu]++ 记录嵌套深度 */
    g_crit_nest[cpu]++;
}

/**
 * @brief 退出内核临界区（嵌套归零时解锁并恢复中断）
 * @details
 * 1. 若 nest 已为 0 则直接返回（防御性）。
 * 2. g_crit_nest[cpu]--；若仍 >0 则仅减计数，保持锁与中断关闭。
 * 3. 嵌套归零时 spin_unlock g_klock，irq_restore 恢复 MIE。
 */
void cgrtos_exit_critical(void)
{
    /* 1. 若 nest 已为 0 则直接返回（防御性） */
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    if (g_crit_nest[cpu] == 0) {
        return;
    }
    /* 2. g_crit_nest[cpu]--；若仍 >0 则仅减计数，保持锁与中断关闭 */
    g_crit_nest[cpu]--;
    /* 3. 嵌套归零时 spin_unlock g_klock，irq_restore 恢复 MIE */
    if (g_crit_nest[cpu] == 0) {
        cgrtos_spin_unlock(&g_klock);
        cgrtos_irq_restore(g_crit_saved[cpu]);
    }
}

/**
 * @brief 查询当前核是否处于临界区内
 * @return 非零表示 g_crit_nest[cpu]>0；否则 0
 * @details
 * 1. 读取 mhartid，返回 g_crit_nest[cpu] 是否大于 0。
 */
int cgrtos_in_critical(void)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    return g_crit_nest[cpu] > 0;
}

/**
 * @brief 查询当前核是否处于 ISR（中断服务）上下文
 * @return 非零表示 g_in_isr[cpu]>0；非法 cpu 返回 0
 * @details
 * 1. 读取 mhartid；若 >= CONFIG_NUM_CORES 返回 0。
 * 2. 返回 g_in_isr[cpu] 嵌套深度是否大于 0。
 */
int cgrtos_in_isr(void)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    if (cpu >= CONFIG_NUM_CORES) {
        return 0;
    }
    return g_in_isr[cpu] > 0;
}

/**
 * @brief 断言失败处理：计数、钩子、打印后停机
 * @param file 触发断言的源文件名
 * @param line 触发断言的行号
 * @details
 * 1. g_assert_count++。
 * 2. 若 g_assert_hook 非空则先调用。
 * 3. 进入临界区打印 [ASSERT FAILED] 文件与行号。
 * 4. 无限 WFI 循环 halt。
 */
void cgrtos_assert_failed(const char *file, int line)
{
    /* 1. 计数 */
    g_assert_count++;
    /* 2. 可选钩子 */
    if (g_assert_hook) {
        g_assert_hook(file, line);
    }
    /* 3. 打印并停机 */
    cgrtos_enter_critical();
    cgrtos_printf("\n[ASSERT FAILED] %s:%d\n", file ? file : "?", line);
    cgrtos_uart_puts("System halted.\n");
    while (1) {
        asm volatile("wfi");
    }
}

/**
 * @brief 获取系统 tick 计数（全局时间基）
 * @return 当前 g_ticks（__ATOMIC_SEQ_CST 顺序一致读）
 * @details
 * 1. __atomic_load_n 以 SEQ_CST 语义读取 g_ticks，与 tick ISR 写保持可见序。
 */
tick_t cgrtos_get_ticks(void)
{
    return __atomic_load_n(&g_ticks, __ATOMIC_SEQ_CST);
}

/**
 * @brief 向次核发送 IPI，完成 SMP 启动握手同步
 * @details
 * 1. 写入 g_boot_sync=0xCAFE5A5A 并内存屏障，通知次核可离开 startup 轮询。
 * 2. 对每个次核 cgrtos_smp_send_ipi 触发 MSIP。
 * 3. 忙等最多 400000 次直至 g_hart1_stage>=4（次核已离开同步阶段）。
 * 4. 打印同步成功或 WARN 日志。
 * @note 仅在 CONFIG_NUM_CORES>1 时编译有效代码
 */
void cgrtos_smp_kick_secondaries(void)
{
#if CONFIG_NUM_CORES > 1
    g_boot_sync = 0xCAFE5A5AU;
    __sync_synchronize();
    for (uint8_t c = 1; c < CONFIG_NUM_CORES; c++) {
        cgrtos_smp_send_ipi(c);
    }
    /* 等待 hart1 离开 startup.S 同步轮询（stage>=4） */
    for (int i = 0; i < 400000 && g_hart1_stage < 4; i++) {
        __sync_synchronize();
    }
    if (g_hart1_stage >= 4) {
        cgrtos_printf("  [SMP] hart1 boot sync ok (stage=%u)\n", g_hart1_stage);
    } else {
        cgrtos_printf("  [SMP] WARN hart1 stage=%u sync=%x\n",
                      g_hart1_stage, g_boot_sync);
    }
#endif
}

/**
 * @brief 内核早期初始化（hart0 在创建用户任务前调用）
 * @details
 * 1. 清零 g_tasks/g_current/g_idle、临界区 nest、yield/remote_tick 等全局状态。
 * 2. 重置 tick、上下文切换计数、负载均衡计数、g_sched_run=0、g_klock=0。
 * 3. 各核 g_in_isr 清零；次核 online 与钩子指针（若启用）清零。
 * 4. 依次 cgrtos_sched_init、arch_init、uart_init、plic_init、clint_init。
 * 5. cgrtos_smp_kick_secondaries 唤醒次核；打印版本横幅；cgrtos_fs_init。
 */
void cgrtos_init(void)
{
    /* 1. 清零 g_tasks/g_current/g_idle、临界区 nest、yield/remote_tick 等全局状态 */
    memset(g_tasks, 0, sizeof(g_tasks));
    memset(g_current, 0, sizeof(g_current));
    memset(g_idle, 0, sizeof(g_idle));
    memset(g_crit_nest, 0, sizeof(g_crit_nest));
    memset((void *)g_yield_pending, 0, sizeof(g_yield_pending));
    memset((void *)g_remote_tick, 0, sizeof(g_remote_tick));

    /* 2. 重置 tick、上下文切换计数、负载均衡计数、g_sched_run=0、g_klock=0 */
    g_task_count = 0;
    g_ticks = 0;
    g_cs_count = 0;
    memset(g_cs_count_core, 0, sizeof(g_cs_count_core));
    g_lb_migrate_count = 0;
    g_lb_steal_count = 0;
    g_sched_run = 0;
    g_klock = 0;
    /* 3. 各核 g_in_isr 清零；次核 online 与钩子指针（若启用）清零 */
    for (uint8_t i = 0; i < CONFIG_NUM_CORES; i++) {
        g_in_isr[i] = 0;
    }
    g_secondary_online = 0;
#if CONFIG_USE_HOOKS
    g_idle_hook = 0;
    g_tick_hook = 0;
    g_malloc_failed_hook = 0;
#endif
    g_assert_hook = 0;
    g_stack_overflow_hook = 0;
    g_assert_count = 0;
    g_stack_overflow_count = 0;
    g_task_create_count = 0;
    g_task_delete_count = 0;

    /* 4. 依次 cgrtos_sched_init、arch_init、uart_init、plic_init、clint_init */
    cgrtos_sched_init();
    cgrtos_arch_init();
    cgrtos_uart_init();
    cgrtos_plic_init();
    cgrtos_clint_init(CONFIG_TICK_RATE_HZ);

    /* 5. cgrtos_smp_kick_secondaries 唤醒次核；打印版本横幅；cgrtos_fs_init */
    cgrtos_smp_kick_secondaries();

    cgrtos_printf("\n\n=== CG-RTOS v%s (SMP multi-policy) ===\n",
                  CGRTOS_VERSION);
    cgrtos_printf("  Cores: %d | Tasks: %d | Tick: %d Hz | Priorities: 0-%d\n",
                  CONFIG_NUM_CORES, CONFIG_MAX_TASKS,
                  CONFIG_TICK_RATE_HZ, CONFIG_MAX_PRIORITY);
    cgrtos_printf("  Features: weighted LB + steal, EDF wheel, CFS/Hybrid, TLSF\n");
    cgrtos_fs_init();
}

/**
 * @brief 启动调度器（hart0 在应用任务创建完毕后调用，不再返回）
 * @details
 * 1. cgrtos_init_idle_tasks 初始化各核 idle；可选 cgrtos_timer_init。
 * 2. g_sched_run=1 发布，次核可进入调度。
 * 3. 扫描 g_tasks 找首个 READY 且 cpu_aff 为 0xFF 或 0 的任务作 first。
 * 4. 若无则 first=g_idle[0]；否则从就绪队列 remove first。
 * 5. g_current[0]=first，state=RUNNING，run_cpu=0。
 * 6. 重新 clint_init 使首个 deadline 相对调度起点；start_first_task 跳转运行。
 */
void cgrtos_start(void)
{
    /* 1. cgrtos_init_idle_tasks 初始化各核 idle；可选 cgrtos_timer_init */
    cgrtos_init_idle_tasks();
#if CONFIG_USE_TIMERS
    cgrtos_timer_init();
#endif
    /* 2. g_sched_run=1 发布，次核可进入调度 */
    g_sched_run = 1;

    /* 3. 扫描 g_tasks 找首个 READY 且 cpu_aff 为 0xFF 或 0 的任务作 first */
    cgrtos_task_t *first = 0;
    for (uint32_t i = 0; i < CONFIG_MAX_TASKS; i++) {
        cgrtos_task_t *task = &g_tasks[i];
        if (task->id == 0 || task->state != TASK_READY) {
            continue;
        }
        if (task->cpu_aff == 0xFF || task->cpu_aff == 0) {
            first = task;
            break;
        }
    }
    /* 4. 若无则 first=g_idle[0]；否则从就绪队列 remove first */
    if (!first) {
        first = &g_idle[0];
    } else {
        cgrtos_sched_remove_ready(first);
    }

    /* 5. g_current[0]=first，state=RUNNING，run_cpu=0 */
    g_current[0] = first;
    first->state = TASK_RUNNING;
    first->run_cpu = 0;

    /* 6. 重新 clint_init 使首个 deadline 相对调度起点；start_first_task 跳转运行 */
    cgrtos_clint_init(CONFIG_TICK_RATE_HZ);
    start_first_task(first->sp);
}

/**
 * @brief 次核进入调度器（secondary_main 调用，不再返回）
 * @param hartid 硬件线程 ID（1..N-1）
 * @details
 * 1. hartid 越界则返回。
 * 2. 自旋等待 g_sched_run==1（hart0 已 cgrtos_start）。
 * 3. 本地 clint_init 启用片内时间片；set_csr MSIE 允许 IPI。
 * 4. g_current[hartid]=idle[hartid]，idle state=RUNNING。
 * 5. 原子发布 g_secondary_online=1；start_first_task 运行 idle。
 * @note g_ticks 仍仅由 hart0 递增
 */
void cgrtos_start_secondary(int hartid)
{
    /* 1. hartid 越界则返回 */
    if (hartid >= CONFIG_NUM_CORES) {
        return;
    }

    /* 2. 自旋等待 g_sched_run==1（hart0 已 cgrtos_start） */
    while (!__atomic_load_n(&g_sched_run, __ATOMIC_SEQ_CST)) {
        __sync_synchronize();
    }

    /* 3. 本地 clint_init 启用片内时间片；set_csr MSIE 允许 IPI */
    cgrtos_clint_init(CONFIG_TICK_RATE_HZ);
    set_csr_bits(mie, 0x8); /* MSIE */

    /* 4. g_current[hartid]=idle[hartid]，idle state=RUNNING */
    g_current[hartid] = &g_idle[hartid];
    g_idle[hartid].state = TASK_RUNNING;
    /* 5. 原子发布 g_secondary_online=1；start_first_task 运行 idle */
    __atomic_store_n(&g_secondary_online, 1, __ATOMIC_SEQ_CST);
    start_first_task(g_idle[hartid].sp);
}

/**
 * @brief 经 UART 转储 CG-RTOS 运行时统计与任务列表
 * @details
 * 1. 打印版本、uptime、存活任务、上下文切换。
 * 2. 打印各核 ready/负载、LB、堆、IPC 计数。
 * 3. 打印 creates/deletes/asserts/stack_overflows。
 * 4. 遍历任务表打印明细（含 exec / hwm）。
 */
void cgrtos_stats_dump(void)
{
    cgrtos_runtime_stats_t st;
    cgrtos_stats_get(&st);

    /* 1. 版本与 uptime */
    cgrtos_uart_puts("\n=== CG-RTOS Runtime Stats ===\n");
    cgrtos_printf("  Version     : %s\n", CGRTOS_VERSION);
    cgrtos_printf("  Uptime      : %lu ticks (%lu ms)\n",
                  (unsigned long)st.uptime_ticks,
                  (unsigned long)(st.uptime_ticks * portTICK_PERIOD_MS));
    cgrtos_printf("  Tasks alive : %u / %u (create=%u delete=%u)\n",
                  st.tasks_alive, CONFIG_MAX_TASKS,
                  st.task_creates, st.task_deletes);
    cgrtos_printf("  Context sw  : %u (core0=%u core1=%u)\n",
                  st.context_switches,
                  st.context_switches_core[0],
                  CONFIG_NUM_CORES > 1 ? st.context_switches_core[1] : 0);
    /* 2. 负载与堆 */
    cgrtos_printf("  Ready/Load  : c0 r=%u L=%u | c1 r=%u L=%u\n",
                  cgrtos_sched_ready_count(0), cgrtos_sched_core_load(0),
                  CONFIG_NUM_CORES > 1 ? cgrtos_sched_ready_count(1) : 0,
                  CONFIG_NUM_CORES > 1 ? cgrtos_sched_core_load(1) : 0);
    cgrtos_printf("  LB migrate  : %u | steal: %u | secondary=%u\n",
                  st.lb_migrate, st.lb_steal, g_secondary_online);
    cgrtos_printf("  Free heap   : %lu bytes (min ever: %lu)\n",
                  st.free_heap, st.min_free_heap);
    cgrtos_printf("  Semaphores  : %u | Mutexes: %u | Queues: %u\n",
                  g_sem_cnt, g_mtx_cnt, g_q_cnt);
    /* 3. 安全计数 */
    cgrtos_printf("  Asserts     : %u | Stack overflows: %u\n",
                  st.asserts, st.stack_overflows);

    /* 4. 任务列表 */
    cgrtos_uart_puts("  Task list:\n");
    for (uint32_t i = 0; i < CONFIG_MAX_TASKS; i++) {
        cgrtos_task_t *task = &g_tasks[i];
        if (task->id == 0) {
            continue;
        }
        static const char *state_names[] = {
            "READY", "RUN", "BLOCK", "SUSP", "DEL"
        };
        static const char *pol_names[] = {
            "RR", "PRI", "CFS", "EDF", "HYB"
        };
        const char *stn = (task->state <= TASK_DELETED) ?
                          state_names[task->state] : "?";
        const char *pol = (task->policy <= SCHED_HYBRID) ?
                          pol_names[task->policy] : "?";
        cgrtos_printf("    [%u] %s id=%lu prio=%u pol=%s state=%s "
                      "cpu=%d vr=%lu dl=%lu exec=%lu hwm=%u\n",
                      i, task->name, task->id, task->prio, pol, stn, task->run_cpu,
                      (unsigned long)task->vruntime, (unsigned long)task->deadline,
                      (unsigned long)task->exec,
                      cgrtos_task_get_stack_high_water_mark(task->id));
    }
    cgrtos_uart_puts("=============================\n\n");
}

/**
 * @brief 填充运行时统计快照
 * @param out 输出结构；NULL 则直接返回
 * @details
 * 1. 校验 out 非空。
 * 2. 复制全局计数器与堆水位、uptime、存活任务数。
 */
void cgrtos_stats_get(cgrtos_runtime_stats_t *out)
{
    if (!out) {
        return;
    }
    /* 1-2. 填充快照字段 */
    memset(out, 0, sizeof(*out));
    out->uptime_ticks = cgrtos_get_ticks();
    out->tasks_alive = g_task_count;
    out->context_switches = g_cs_count;
    for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
        out->context_switches_core[c] = g_cs_count_core[c];
    }
    out->lb_migrate = g_lb_migrate_count;
    out->lb_steal = g_lb_steal_count;
    out->task_creates = g_task_create_count;
    out->task_deletes = g_task_delete_count;
    out->stack_overflows = g_stack_overflow_count;
    out->asserts = g_assert_count;
    out->free_heap = cgrtos_get_free_heap();
    out->min_free_heap = cgrtos_get_min_free_heap();
}

/**
 * @brief ISR 入口钩子：递增当前核 ISR 嵌套计数
 * @details
 * 1. 读取 mhartid；合法 cpu 则 g_in_isr[cpu]++。
 * 2. 供 cgrtos_in_isr 判断当前是否在中断上下文。
 */
void cgrtos_isr_enter(void)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    if (cpu < CONFIG_NUM_CORES) {
        g_in_isr[cpu]++;
    }
}

/**
 * @brief ISR 出口钩子：递减当前核 ISR 嵌套计数
 * @details
 * 1. 读取 mhartid；若 g_in_isr[cpu]>0 则递减。
 * 2. 实际任务切换在 trap_vector 中经 cgrtos_sched_switch_from_trap 完成，此处不 yield。
 */
void cgrtos_isr_exit(void)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    if (cpu < CONFIG_NUM_CORES && g_in_isr[cpu] > 0) {
        g_in_isr[cpu]--;
    }
    /* 实际切换在 trap_vector 的 cgrtos_sched_switch_from_trap 中发生 */
}
