/**
 * @file cgrtos.c
 * @brief 内核全局变量、临界区与 SMP 启动
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details 提供 CG-RTOS 内核全局状态、可嵌套临界区、自旋锁、
 *          系统初始化/启动、次核启动、运行时统计及 ISR 进出跟踪。
 *
 * 启动流程（另见 startup.S）：
 *   hart0: cgrtos_init → 唤醒次核 MSIP → 应用创建任务 → cgrtos_start
 *   hart1: secondary_main → 等待 g_sched_run → idle
 *
 * 临界区：每核可嵌套 IRQ 关闭 + 全局自旋锁 g_klock。
 */
#include "cgrtos.h"
#include "hal_board.h"
#include <string.h>
#if CONFIG_USE_VFS
#include "vfs.h"
#endif

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
/**
 * @brief 次核在线位图：bit N（N≥1）表示 hart N 已进入调度器
 */
volatile uint32_t g_secondary_online;
/**
 * @brief 各 hart 启动阶段面包屑（须在 .data，避免 BSS 清零抹掉次核进度）
 * @note startup.S 按 hartid 索引；kick 等待 1..CONFIG_NUM_CORES-1 均 ≥4
 */
volatile uint32_t g_hart_stage[CONFIG_MAX_CORES] __attribute__((section(".data")));

#if CONFIG_USE_HOOKS
/** @brief idle 任务钩子函数指针 */
cgrtos_hook_fn_t g_idle_hook;
/** @brief tick 钩子函数指针 */
cgrtos_hook_fn_t g_tick_hook;
/** @brief malloc 失败钩子函数指针 */
cgrtos_malloc_failed_hook_t g_malloc_failed_hook;

/**
 * @brief 注册 idle 任务钩子函数
 * @details 将全局 g_idle_hook 设为 hook；idle 任务每轮循环开头若 hook 非空则调用。
 * @param[in] hook 钩子回调；NULL 表示清除
 * @return 无
 * @retval 无
 * @note CONFIG_USE_HOOKS 编译
 * @warning 钩子须短小非阻塞
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_set_idle_hook(cgrtos_hook_fn_t hook)
{
    g_idle_hook = hook;
}

/**
 * @brief 注册系统 tick 钩子函数
 * @details 将 g_tick_hook 设为 hook；tick ISR 路径在适当时机调用。
 * @param[in] hook 钩子回调；NULL 清除
 * @return 无
 * @retval 无
 * @note CONFIG_USE_HOOKS
 * @warning 钩子内勿阻塞
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_set_tick_hook(cgrtos_hook_fn_t hook)
{
    g_tick_hook = hook;
}

/**
 * @brief 注册堆分配失败钩子
 * @details 保存 g_malloc_failed_hook；malloc 失败路径调用。
 * @param[in] hook 回调；NULL 清除
 * @return 无
 * @retval 无
 * @note CONFIG_USE_HOOKS
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
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
 * @details 保存 g_assert_hook；assert 失败时在 halt 前调用。
 * @param[in] hook 回调；NULL 清除
 * @return 无
 * @retval 无
 * @note 钩子返回后仍 halt
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_set_assert_hook(cgrtos_assert_hook_t hook)
{
    g_assert_hook = hook;
}

/**
 * @brief 注册栈溢出钩子
 * @details 保存 g_stack_overflow_hook；溢出检测后调用，默认仍 assert halt。
 * @param[in] hook 回调；NULL 清除
 * @return 无
 * @retval 无
 * @note 可与 CONFIG_CHECK_STACK_OVERFLOW 配合
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
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
 * @details 循环 __sync_lock_test_and_set 直至成功；__sync_synchronize 屏障。
 * @param[inout] lock 自旋锁指针
 * @return 无
 * @retval 无
 * @note 临界区外层与 ready_lock 等使用
 * @warning 持锁期间勿阻塞
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_spin_lock(spinlock_t *lock)
{
    while (__sync_lock_test_and_set(lock, 1)) {
    }
    __sync_synchronize();
}

/**
 * @brief 释放自旋锁
 * @details __sync_synchronize 后 __sync_lock_release 写 0。
 * @param[inout] lock 自旋锁指针
 * @return 无
 * @retval 无
 * @note 与 spin_lock 配对
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_spin_unlock(spinlock_t *lock)
{
    __sync_synchronize();
    __sync_lock_release(lock);
}

/**
 * @brief 保存并屏蔽可屏蔽 IRQ（委托 arch_irq_save）
 * @details 架构细节见 kernel/arch_port.h。
 * @return 架构相关 flags
 * @retval 任意 uint64_t 供 irq_restore 使用的保存值
 * @note 与 irq_restore 配对
 * @warning 必须与 irq_restore 成对，禁止跨任务传递 flags
 * @attention ✅ ISR；❌ block/switch
 */
uint64_t cgrtos_irq_save(void)
{
    return arch_irq_save();
}

/**
 * @brief 恢复中断屏蔽状态（委托 arch_irq_restore）
 * @details 写回 flags。
 * @param[in] flags cgrtos_irq_save 返回值
 * @return 无
 * @retval 无
 * @note 与 irq_save 配对
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_irq_restore(uint64_t flags)
{
    arch_irq_restore(flags);
}

/**
 * @brief 进入内核临界区（可嵌套；首层关中断并持全局锁）
 * @details 最外层 irq_save + spin_lock g_klock + safety_on_crit_enter；nest++。
 * @return 无
 * @retval 无
 * @note 必须与 exit_critical 配对
 * @warning 持 g_klock 时 EDF kick 推迟
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_enter_critical(void)
{
    /* 1. 读取当前 hartid 作为 cpu 索引 */
    uint8_t cpu = arch_cpu_id();
    /* 2. 若 g_crit_nest[cpu]==0（最外层），irq_save 关中断并 spin_lock g_klock */
    if (g_crit_nest[cpu] == 0) {
        g_crit_saved[cpu] = cgrtos_irq_save();
        cgrtos_spin_lock(&g_klock);
        cgrtos_safety_on_crit_enter(cpu);
    }
    /* 3. g_crit_nest[cpu]++ 记录嵌套深度 */
    g_crit_nest[cpu]++;
}

/**
 * @brief 退出内核临界区（嵌套归零时解锁并恢复中断）
 * @details nest--；归零则 safety_on_crit_exit、unlock、irq_restore、edf_kick_flush。
 * @return 无
 * @retval 无
 * @note 最外层退出时补发推迟的 EDF kick
 * @warning nest 已为 0 则防御返回
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_exit_critical(void)
{
    /* 1. 若 nest 已为 0 则直接返回（防御性） */
    uint8_t cpu = arch_cpu_id();
    if (g_crit_nest[cpu] == 0) {
        return;
    }
    /* 2. g_crit_nest[cpu]--；若仍 >0 则仅减计数，保持锁与中断关闭 */
    g_crit_nest[cpu]--;
    /* 3. 嵌套归零时 spin_unlock g_klock，irq_restore 恢复 MIE */
    if (g_crit_nest[cpu] == 0) {
        cgrtos_safety_on_crit_exit(cpu);
        cgrtos_spin_unlock(&g_klock);
        cgrtos_irq_restore(g_crit_saved[cpu]);
        /* EDF 入队若在临界区内被推迟，此处补发 kick */
        cgrtos_sched_edf_kick_flush();
    }
}

/**
 * @brief 查询当前核是否处于临界区内
 * @details 返回 g_crit_nest[cpu]>0。
 * @return 非零表示在临界区
 * @retval 1 在临界区
 * @retval 0 不在
 * @note 调度 kick 推迟判断等
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_in_critical(void)
{
    uint8_t cpu = arch_cpu_id();
    return g_crit_nest[cpu] > 0;
}

/**
 * @brief 查询当前核是否处于 ISR 上下文
 * @details mhartid 合法则返回 g_in_isr[cpu]>0。
 * @return 非零表示在 ISR
 * @retval 1 在 ISR
 * @retval 0 任务上下文或非法 cpu
 * @note yield/block 路径分支
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_in_isr(void)
{
    uint8_t cpu = arch_cpu_id();
    if (cpu >= CONFIG_NUM_CORES) {
        return 0;
    }
    return g_in_isr[cpu] > 0;
}

/**
 * @brief 断言失败处理：计数、钩子、打印后停机
 * @details g_assert_count++；可选 hook；enter_critical 打印；WFI 死循环。
 * @param[in] file 源文件名
 * @param[in] line 行号
 * @return 无
 * @retval 无
 * @note 不返回
 * @warning 系统 halt
 * @attention ✅ ISR；❌ block/switch
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
        arch_cpu_wait();
    }
}

/**
 * @brief 获取系统 tick 计数（全局时间基）
 * @details __atomic_load_n g_ticks SEQ_CST 读。
 * @return 当前 tick 值
 * @retval >=0 全局 tick
 * @note 与 tick ISR 写保持可见序
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
tick_t cgrtos_get_ticks(void)
{
    return __atomic_load_n(&g_ticks, __ATOMIC_SEQ_CST);
}

/**
 * @brief 向次核发送 IPI，完成 SMP 启动握手同步
 * @details 写 boot_sync 魔数；对各次核 MSIP；忙等 g_hart_stage>=4；打印结果。
 * @return 无
 * @retval 无
 * @note 单核为空操作
 * @warning 仅 init 早期调用
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_smp_kick_secondaries(void)
{
#if CONFIG_NUM_CORES > 1
    g_boot_sync = 0xCAFE5A5AU;
    __sync_synchronize();
    for (uint8_t c = 1; c < CONFIG_NUM_CORES; c++) {
        cgrtos_smp_send_ipi(c);
    }
    /* 等待全部次核离开 startup.S 同步轮询（stage>=4） */
    for (int i = 0; i < 400000; i++) {
        __sync_synchronize();
        int all_ok = 1;
        for (uint8_t c = 1; c < CONFIG_NUM_CORES; c++) {
            if (g_hart_stage[c] < 4) {
                all_ok = 0;
                break;
            }
        }
        if (all_ok) {
            break;
        }
    }
    for (uint8_t c = 1; c < CONFIG_NUM_CORES; c++) {
        if (g_hart_stage[c] >= 4) {
            cgrtos_printf("  [SMP] hart%u boot sync ok (stage=%u)\n",
                          c, g_hart_stage[c]);
        } else {
            cgrtos_printf("  [SMP] WARN hart%u stage=%u sync=%x\n",
                          c, g_hart_stage[c], g_boot_sync);
        }
    }
#else
    /* 单核：无需 kick */
#endif
}

/**
 * @brief 内核早期初始化（hart0 在创建用户任务前调用）
 * @details 清零全局状态；sched_init；hal_board_init；kick 次核；打印横幅；fs_init；可选 mpu_init。
 * @return 无
 * @retval 无
 * @note hart0 专用
 * @warning 未完成前勿启动调度
 * @attention ❌ ISR；❌ block/switch
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

    /* 4. 调度器 + 统一 HAL 板级外设初始化 */
    cgrtos_sched_init();
    hal_board_init();

    /* 5. cgrtos_smp_kick_secondaries 唤醒次核；打印版本横幅；cgrtos_fs_init */
    cgrtos_smp_kick_secondaries();

    cgrtos_printf("\n\n=== CG-RTOS v%s (SMP multi-policy) ===\n",
                  CGRTOS_VERSION);
    cgrtos_printf("  Cores: %d (build) | Tasks: %d | Tick: %d Hz | Priorities: 0-%d\n",
                  CONFIG_NUM_CORES, CONFIG_MAX_TASKS,
                  CONFIG_TICK_RATE_HZ, CONFIG_MAX_PRIORITY);
    cgrtos_printf("  HAL board: %s | devices=%d\n",
                  HAL_BOARD, hal_device_count());
    cgrtos_printf("  Features: M1-M6 EDF-heap DPCP klog mempool safety\n");
    cgrtos_fs_init();
#if CONFIG_USE_VFS
    vfs_init();
#endif
#if CONFIG_USE_MPU
    (void)cgrtos_mpu_init();
#endif
}

/**
 * @brief 启动调度器（hart0 应用任务创建后调用，不再返回）
 * @details init_idle；timer_init；g_sched_run=1；选 first READY；clint_init；start_first_task。
 * @return 无
 * @retval 无
 * @note 不返回用户代码
 * @warning 须先 cgrtos_init
 * @attention ❌ ISR；✅ block/switch
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
 * @details 非法 hart WFI；等 g_sched_run；clint_init+MSIE；current=idle；置 online；start_first_task。
 * @param[in] hartid 硬件线程 ID（1..N-1）
 * @return 无
 * @retval 无
 * @note g_ticks 仍仅 hart0 递增
 * @warning 越界 hart 永久 WFI
 * @attention ❌ ISR；✅ block/switch
 */
void cgrtos_start_secondary(int hartid)
{
    /* 1. 越界 hart：镜像可拉起 MAX_CORES 个 hart，但本构建未启用 */
    if (hartid <= 0 || hartid >= CONFIG_NUM_CORES) {
        while (1) {
            arch_cpu_wait();
        }
    }

    /* 2. 自旋等待 g_sched_run==1（hart0 已 cgrtos_start） */
    while (!__atomic_load_n(&g_sched_run, __ATOMIC_SEQ_CST)) {
        __sync_synchronize();
    }

    /* 3. 本地 timer init；使能本核 IPI 接收 */
    cgrtos_clint_init(CONFIG_TICK_RATE_HZ);
    arch_cpu_enable_ipi();

    /* 4. g_current[hartid]=idle[hartid]，idle state=RUNNING */
    g_current[hartid] = &g_idle[hartid];
    g_idle[hartid].state = TASK_RUNNING;
    /* 5. 原子置位次核在线位；start_first_task 运行 idle */
    __atomic_or_fetch(&g_secondary_online, (1U << hartid), __ATOMIC_SEQ_CST);
    start_first_task(g_idle[hartid].sp);
}

/**
 * @brief 经 UART 转储 CG-RTOS 运行时统计与任务列表
 * @details stats_get 后打印版本、uptime、切换、负载、堆、IPC、断言及任务明细。
 * @return 无
 * @retval 无
 * @note 调试/CLI 使用
 * @warning 输出量大
 * @attention ❌ ISR；❌ block/switch
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
    cgrtos_printf("  Context sw  : %u", st.context_switches);
    for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
        cgrtos_printf(" c%u=%u", c, st.context_switches_core[c]);
    }
    cgrtos_printf("\n");
    /* 2. 负载与堆 */
    cgrtos_printf("  Ready/Load  :");
    for (uint8_t c = 0; c < CONFIG_NUM_CORES; c++) {
        cgrtos_printf(" c%u r=%u L=%u", c,
                      cgrtos_sched_ready_count(c), cgrtos_sched_core_load(c));
        if (c + 1 < CONFIG_NUM_CORES) {
            cgrtos_printf(" |");
        }
    }
    cgrtos_printf("\n");
    cgrtos_printf("  LB migrate  : %u | steal: %u | secondary_mask=0x%x\n",
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
            "READY", "RUN", "BLOCK", "SUSP", "TERM", "DEL"
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
 * @details out 非空则 memset 后复制全局计数器、堆水位、uptime、可选 sched_stats。
 * @param[out] out 输出结构；NULL 则返回
 * @return 无
 * @retval 无
 * @note stats_dump 与外部监控共用
 * @warning out 为 NULL 直接返回
 * @attention ✅ ISR；❌ block/switch
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
#if CONFIG_SCHED_STATS
    {
        tick_t ml = 0;
        uint32_t ns = 0;
        cgrtos_sched_stats_get(&ml, &ns);
        out->max_sched_latency = ml;
        out->sched_latency_samples = ns;
    }
#endif
}

/**
 * @brief ISR 入口钩子：递增当前核 ISR 嵌套计数
 * @details 合法 cpu 则 g_in_isr[cpu]++；供 cgrtos_in_isr 判断。
 * @return 无
 * @retval 无
 * @note trap_vector 入口调用
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_isr_enter(void)
{
    uint8_t cpu = arch_cpu_id();
    if (cpu < CONFIG_NUM_CORES) {
        g_in_isr[cpu]++;
        CGRTOS_TRACE(CGRTOS_TRACE_ISR_ENTER, g_in_isr[cpu], 0);
    }
}

/**
 * @brief ISR 出口钩子：递减当前核 ISR 嵌套计数
 * @details g_in_isr[cpu]--；实际切换在 switch_from_trap。
 * @return 无
 * @retval 无
 * @note 与 isr_enter 配对
 * @warning 嵌套须对称
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_isr_exit(void)
{
    uint8_t cpu = arch_cpu_id();
    if (cpu < CONFIG_NUM_CORES && g_in_isr[cpu] > 0) {
        g_in_isr[cpu]--;
        CGRTOS_TRACE(CGRTOS_TRACE_ISR_EXIT, g_in_isr[cpu], 0);
    }
    /* 实际切换在 trap_vector 的 cgrtos_sched_switch_from_trap 中发生 */
}
