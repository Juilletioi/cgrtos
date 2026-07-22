/**
 * @file stress_cases.c
 * @brief CG-RTOS 全 feature 并发压力测试实现
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 同时压测：SMP / RR·CFS·EDF·Hybrid·Priority / sem·mutex·queue·event·notify /
 * 软定时器 / TLSF / 延时超时 / 亲和与负载均衡 / 挂起恢复删除 / 临界区。
 * 由 APP=stress 或 CLI `run stress` 调用 stress_run()。
 */
#include "../kernel/cgrtos.h"
#include "stress_cases.h"

/* ---- knobs (keep within CONFIG_MAX_TASKS / pool limits) ---- */
#define STRESS_MS           1200
#define N_RR                3
#define N_CFS               3
#define N_EDF               2
#define N_HYBRID            2
#define N_SEM_PAIR          2
#define N_Q_PAIR            2
#define N_MTX               2
#define N_EVT_WAIT          2
#define N_NOTIFY            2
#define N_HEAP              2
#define N_TIMERS            4
#define HEAP_CHURN_MAX      6
#define LIFE_MAX_OPS        25

static volatile int g_pass;
static volatile int g_fail;
static volatile int g_stop;

static volatile uint32_t g_rr_ops;
static volatile uint32_t g_cfs_ops;
static volatile uint32_t g_edf_ok;
static volatile uint32_t g_edf_miss;
static volatile uint32_t g_hyb_ops;
static volatile uint32_t g_sem_ops;
static volatile uint32_t g_q_ops;
static volatile uint32_t g_mtx_ops;
static volatile uint32_t g_evt_ops;
static volatile uint32_t g_notify_ops;
static volatile uint32_t g_heap_ops;
static volatile uint32_t g_tmr_fires;
static volatile uint32_t g_life_ops;
static volatile uint32_t g_core0_hits;
static volatile uint32_t g_core1_hits;
static volatile uint32_t g_lb_migrated;
static volatile uint32_t g_yield_storm;

static cgrtos_sem_t *g_sem[N_SEM_PAIR];
static cgrtos_queue_t *g_q[N_Q_PAIR];
static cgrtos_mutex_t *g_mtx;
static cgrtos_event_group_t *g_eg;
static cgrtos_task_t *g_notify_tgt[N_NOTIFY];
static cgrtos_timer_t *g_tmrs[N_TIMERS];

/**
 * @brief 记录单条压力断言结果并更新全局 pass/fail 计数
 * @details cond 为真打印 [PASS] 并递增 g_pass，否则 [FAIL] 并递增 g_fail。
 * @param[in] name 断言描述字符串
 * @param[in] cond 条件（非零为通过）
 * @return 无
 * @retval 无
 * @note 供 stress_run 内各 expect 调用复用
 * @warning 多任务并发调用计数无原子保护
 * @attention ❌ ISR 勿依赖打印副作用；❌ 不阻塞
 * @internal
 */
static void expect(const char *name, int cond)
{
    if (cond) {
        g_pass++;
        cgrtos_printf("  [PASS] %s\n", name);
    } else {
        g_fail++;
        cgrtos_printf("  [FAIL] %s\n", name);
    }
}

/**
 * @brief 按当前 hart 递增对应核命中计数
 * @details 读 mhartid；hart0 递增 g_core0_hits，否则递增 g_core1_hits。
 * @return 无
 * @retval 无
 * @note 各 worker 热路径调用以验证 SMP 调度
 * @warning 无
 * @attention ✅ 任意任务上下文；❌ 不阻塞
 * @internal
 */
static void bump_core(void)
{
    if ((uint8_t)read_csr(mhartid) == 0) {
        __atomic_fetch_add(&g_core0_hits, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&g_core1_hits, 1, __ATOMIC_RELAXED);
    }
}

/**
 * @brief 等待全局停止标志后永久 yield 以便 runner 删除
 * @details 轮询 g_stop，期间 cgrtos_delay_ms(20)；置位后进入无限 yield 循环。
 * @return 无（永不返回）
 * @retval 无
 * @note worker 退出主循环后调用，避免自删竞态
 * @warning 依赖 runner 提升优先级并 task_delete
 * @attention ❌ ISR；✅ 阻塞 delay/yield
 * @internal
 */
static void wait_until_stop(void)
{
    while (!g_stop) {
        cgrtos_delay_ms(20);
    }
    /* Stay schedulable so the high-prio runner can delete us promptly. */
    while (1) {
        cgrtos_task_yield();
    }
}

/* ---------------- workers ---------------- */

/**
 * @brief RR 调度压力 worker：忙算 + yield 循环
 * @details 直至 g_stop：bump_core、递增 g_rr_ops、短循环算术、cgrtos_task_yield。
 * @param[in] arg 未使用
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 由 stress_run 以 SCHED_RR 创建
 * @warning 无
 * @attention ❌ ISR；✅ yield
 * @internal
 */
static void rr_worker(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bump_core();
        __atomic_fetch_add(&g_rr_ops, 1, __ATOMIC_RELAXED);
        volatile int x = 0;
        for (int i = 0; i < 80; i++) {
            x += i;
        }
        (void)x;
        cgrtos_task_yield();
    }
    wait_until_stop();
}

/**
 * @brief CFS 调度压力 worker：仅 yield 循环
 * @details 直至 g_stop：bump_core、递增 g_cfs_ops、cgrtos_task_yield。
 * @param[in] arg 未使用
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 双核时通常亲和 hart1
 * @warning 单核下可能被 PRI 饿死（stress 软通过）
 * @attention ❌ ISR；✅ yield
 * @internal
 */
static void cfs_worker(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bump_core();
        __atomic_fetch_add(&g_cfs_ops, 1, __ATOMIC_RELAXED);
        cgrtos_task_yield();
    }
    wait_until_stop();
}

/**
 * @brief EDF 周期任务 worker：设 deadline 并校验是否错过
 * @details arg 为周期 tick；循环设 period/deadline、短算术、按时 delay 或计 miss。
 * @param[in] arg 周期 tick 数（uintptr_t 传递；最小钳制为 5）
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 成功计 g_edf_ok，超时计 g_edf_miss
 * @warning deadline 设置依赖 self 有效
 * @attention ❌ ISR；✅ delay/set_deadline
 * @internal
 */
static void edf_worker(void *arg)
{
    tick_t period = (tick_t)(uintptr_t)arg;
    if (period < 5) {
        period = 5;
    }
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *self = g_current[cpu];
    tick_t wake = cgrtos_get_ticks();
    if (self) {
        cgrtos_task_set_period(self->id, period);
    }
    while (!g_stop) {
        bump_core();
        wake += period;
        if (self) {
            cgrtos_task_set_deadline(self->id, wake);
        }
        volatile int x = 0;
        for (int i = 0; i < 120; i++) {
            x += i;
        }
        (void)x;
        tick_t done = cgrtos_get_ticks();
        if (done <= wake) {
            __atomic_fetch_add(&g_edf_ok, 1, __ATOMIC_RELAXED);
            if (done < wake) {
                cgrtos_delay(wake - done);
            }
        } else {
            __atomic_fetch_add(&g_edf_miss, 1, __ATOMIC_RELAXED);
            wake = cgrtos_get_ticks() + period;
        }
    }
    wait_until_stop();
}

/**
 * @brief Hybrid 调度 worker：RT 分支 delay，非 RT 分支 yield
 * @details arg 非零走 cgrtos_delay(1)，否则 cgrtos_task_yield；递增 g_hyb_ops。
 * @param[in] arg RT 标志（uintptr_t：0=非 RT，非 0=RT）
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note hyb0 使用高于 RT 阈值的 prio
 * @warning RT 路径占用 CPU 时间片
 * @attention ❌ ISR；✅ delay/yield
 * @internal
 */
static void hybrid_worker(void *arg)
{
    int rt = (int)(uintptr_t)arg;
    while (!g_stop) {
        bump_core();
        __atomic_fetch_add(&g_hyb_ops, 1, __ATOMIC_RELAXED);
        if (rt) {
            cgrtos_delay(1);
        } else {
            cgrtos_task_yield();
        }
    }
    wait_until_stop();
}

/**
 * @brief 信号量生产者 worker：循环 give
 * @details 索引 arg 取 g_sem[idx]；成功 give 递增 g_sem_ops 后 yield。
 * @param[in] arg 信号量对索引（uintptr_t）
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 与 sem_cons 成对压测
 * @warning 无
 * @attention ❌ ISR；✅ sem API/yield
 * @internal
 */
static void sem_prod(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;
    cgrtos_sem_t *s = g_sem[idx];
    while (!g_stop) {
        bump_core();
        if (cgrtos_sem_give(s) == pdPASS) {
            __atomic_fetch_add(&g_sem_ops, 1, __ATOMIC_RELAXED);
        }
        cgrtos_task_yield();
    }
    wait_until_stop();
}

/**
 * @brief 信号量消费者 worker：带超时 take
 * @details 索引 arg 取 g_sem[idx]；take 5ms 超时成功则递增 g_sem_ops。
 * @param[in] arg 信号量对索引（uintptr_t）
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note stop 阶段 runner 会额外 give 以唤醒
 * @warning 无
 * @attention ❌ ISR；✅ 阻塞 take
 * @internal
 */
static void sem_cons(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;
    cgrtos_sem_t *s = g_sem[idx];
    while (!g_stop) {
        bump_core();
        if (cgrtos_sem_take(s, portMS_TO_TICK(5)) == pdPASS) {
            __atomic_fetch_add(&g_sem_ops, 1, __ATOMIC_RELAXED);
        }
    }
    wait_until_stop();
}

/**
 * @brief 队列生产者 worker：循环 send uint32_t
 * @details 索引 arg 取 g_q[idx]；send 成功递增 g_q_ops 并 bump 本地 v。
 * @param[in] arg 队列对索引（uintptr_t）
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 与 q_cons 成对压测
 * @warning 无
 * @attention ❌ ISR；✅ 队列 send（可能阻塞）
 * @internal
 */
static void q_prod(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;
    cgrtos_queue_t *q = g_q[idx];
    uint32_t v = (uint32_t)idx;
    while (!g_stop) {
        bump_core();
        if (cgrtos_queue_send(q, &v, portMS_TO_TICK(5)) == pdPASS) {
            __atomic_fetch_add(&g_q_ops, 1, __ATOMIC_RELAXED);
            v++;
        }
    }
    wait_until_stop();
}

/**
 * @brief 队列消费者 worker：带超时 receive
 * @details 索引 arg 取 g_q[idx]；receive 成功递增 g_q_ops。
 * @param[in] arg 队列对索引（uintptr_t）
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note stop 阶段 runner 会 poke 空消息唤醒
 * @warning 无
 * @attention ❌ ISR；✅ 阻塞 receive
 * @internal
 */
static void q_cons(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;
    cgrtos_queue_t *q = g_q[idx];
    uint32_t out = 0;
    while (!g_stop) {
        bump_core();
        if (cgrtos_queue_receive(q, &out, portMS_TO_TICK(5)) == pdPASS) {
            __atomic_fetch_add(&g_q_ops, 1, __ATOMIC_RELAXED);
        }
    }
    wait_until_stop();
}

/**
 * @brief 互斥锁压力 worker：加锁临界区算术后解锁
 * @details lock 10ms 超时；持锁短循环后 unlock 并递增 g_mtx_ops。
 * @param[in] arg 未使用
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 共享全局 g_mtx
 * @warning 锁竞争可能导致部分轮次 lock 超时
 * @attention ❌ ISR；✅ 阻塞 lock
 * @internal
 */
static void mtx_worker(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bump_core();
        if (cgrtos_mutex_lock(g_mtx, portMS_TO_TICK(10)) == pdPASS) {
            volatile int x = 0;
            for (int i = 0; i < 40; i++) {
                x += i;
            }
            (void)x;
            cgrtos_mutex_unlock(g_mtx);
            __atomic_fetch_add(&g_mtx_ops, 1, __ATOMIC_RELAXED);
        }
        cgrtos_task_yield();
    }
    wait_until_stop();
}

/**
 * @brief 事件组等待 worker：wait_bits 位 0x1
 * @details 8ms 超时 wait；收到 0x1 递增 g_evt_ops。
 * @param[in] arg 未使用
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 与 evt_setter 成对
 * @warning 无
 * @attention ❌ ISR；✅ wait_bits 阻塞
 * @internal
 */
static void evt_waiter(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bump_core();
        event_flags_t f = cgrtos_event_group_wait_bits(g_eg, 0x1, 1, 0,
                                                       portMS_TO_TICK(8));
        if (f & 0x1) {
            __atomic_fetch_add(&g_evt_ops, 1, __ATOMIC_RELAXED);
        }
    }
    wait_until_stop();
}

/**
 * @brief 事件组置位 worker：周期 set 0x1
 * @details 循环 set 0x1、递增 g_evt_ops、cgrtos_delay(2)。
 * @param[in] arg 未使用
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 与 evt_waiter 成对
 * @warning 无
 * @attention ❌ ISR；✅ delay/set
 * @internal
 */
static void evt_setter(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bump_core();
        cgrtos_event_group_set(g_eg, 0x1);
        __atomic_fetch_add(&g_evt_ops, 1, __ATOMIC_RELAXED);
        cgrtos_delay(2);
    }
    wait_until_stop();
}

/**
 * @brief task notify 等待 worker
 * @details notify_wait 8ms 超时；成功递增 g_notify_ops。
 * @param[in] arg 索引（未使用，仅传递）
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 与 notify_sender 成对
 * @warning 无
 * @attention ❌ ISR；✅ 阻塞 notify_wait
 * @internal
 */
static void notify_waiter(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;
    while (!g_stop) {
        bump_core();
        uint32_t val = 0;
        if (cgrtos_task_notify_wait(0, 0xFFFFFFFFU, &val, portMS_TO_TICK(8))) {
            __atomic_fetch_add(&g_notify_ops, 1, __ATOMIC_RELAXED);
            (void)idx;
        }
    }
    wait_until_stop();
}

/**
 * @brief task notify 发送 worker
 * @details 向 g_notify_tgt[idx] 发送 eIncrement notify 并递增 g_notify_ops。
 * @param[in] arg 目标索引（uintptr_t）
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 目标句柄由 stress_run 在创建后填充
 * @warning tgt 为 NULL 时跳过发送
 * @attention ❌ ISR；✅ yield/notify
 * @internal
 */
static void notify_sender(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;
    while (!g_stop) {
        bump_core();
        cgrtos_task_t *t = g_notify_tgt[idx];
        if (t) {
            cgrtos_task_notify(t, 1, eIncrement);
            __atomic_fetch_add(&g_notify_ops, 1, __ATOMIC_RELAXED);
        }
        cgrtos_task_yield();
    }
    wait_until_stop();
}

/**
 * @brief 堆分配 churn worker：轮转 malloc/free
 * @details 固定槽 HEAP_CHURN_MAX；按 seed 变化尺寸分配，退出前释放残留。
 * @param[in] arg 分配尺寸种子（uintptr_t）
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 每 8 次 op yield 一次
 * @warning 失败 malloc 仍计 op；勿在 ISR 调用堆 API
 * @attention ❌ ISR；✅ 堆 API/yield
 * @internal
 */
static void heap_churn(void *arg)
{
    uintptr_t seed = (uintptr_t)arg;
    void *slots[HEAP_CHURN_MAX];
    for (int i = 0; i < HEAP_CHURN_MAX; i++) {
        slots[i] = 0;
    }
    uint32_t n = 0;
    while (!g_stop) {
        bump_core();
        int i = (int)(n % HEAP_CHURN_MAX);
        if (slots[i]) {
            cgrtos_free(slots[i]);
            slots[i] = 0;
        } else {
            uint32_t sz = 16U + ((n + (uint32_t)seed) % 7U) * 32U;
            slots[i] = cgrtos_malloc(sz);
            if (slots[i]) {
                ((uint8_t *)slots[i])[0] = (uint8_t)n;
            }
        }
        n++;
        __atomic_fetch_add(&g_heap_ops, 1, __ATOMIC_RELAXED);
        if ((n & 7U) == 0U) {
            cgrtos_task_yield();
        }
    }
    for (int i = 0; i < HEAP_CHURN_MAX; i++) {
        if (slots[i]) {
            cgrtos_free(slots[i]);
        }
    }
    wait_until_stop();
}

/**
 * @brief 软定时器压力回调：递增 g_tmr_fires
 * @details 定时器 daemon 上下文调用；原子递增计数。
 * @param[in] arg 未使用
 * @return 无
 * @retval 无
 * @note 由 stress_run 创建 N_TIMERS 个周期定时器
 * @warning 须保持短小，勿阻塞
 * @attention ✅ 定时器 daemon 上下文；❌ 非 ISR 直调
 * @internal
 */
static void timer_cb(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&g_tmr_fires, 1, __ATOMIC_RELAXED);
}

/**
 * @brief 占位 worker：仅 delay 循环
 * @details life_cycle 创建的临时任务入口；bump_core 后 cgrtos_delay(4)。
 * @param[in] arg 未使用
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 供 life_cycle 反复 create/delete
 * @warning 无
 * @attention ❌ ISR；✅ delay
 * @internal
 */
static void nop_worker(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bump_core();
        cgrtos_delay(4);
    }
    wait_until_stop();
}

/**
 * @brief 任务生命周期压力 worker：create/suspend/resume/delete
 * @details 最多 LIFE_MAX_OPS 次：创建 nop_worker、改 prio/affinity、delete。
 * @param[in] arg 未使用
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 每次迭代 delay_ms(40) 降低 UART 日志开销
 * @warning create 失败则跳过本次
 * @attention ❌ ISR；✅ 创建/删除任务、delay
 * @internal
 */
static void life_cycle(void *arg)
{
    (void)arg;
    uint32_t ops = 0;
    while (!g_stop && ops < LIFE_MAX_OPS) {
        bump_core();
        task_id_t id = cgrtos_task_create("tmp", nop_worker, 0, 2, SCHED_RR);
        if (id != (task_id_t)-1 && id != 0) {
            cgrtos_task_suspend(id);
            cgrtos_task_resume(id);
            cgrtos_task_set_priority(id, 3);
            if (g_secondary_online) {
                cgrtos_task_set_affinity(id, (uint8_t)(id & 1U));
            }
            cgrtos_task_delete(id);
            __atomic_fetch_add(&g_life_ops, 1, __ATOMIC_RELAXED);
            ops++;
        }
        /* UART create logs are expensive — keep this rare. */
        cgrtos_delay_ms(40);
    }
    wait_until_stop();
}

/**
 * @brief yield 风暴 worker：批量 yield + 临界区进出
 * @details 每轮 32 次 yield（计 g_yield_storm）后 enter/exit critical。
 * @param[in] arg 未使用
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 压测调度器与 g_cs_count
 * @warning 临界区须配对
 * @attention ❌ ISR；✅ yield/临界区
 * @internal
 */
static void yield_storm(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bump_core();
        for (int i = 0; i < 32; i++) {
            cgrtos_task_yield();
            __atomic_fetch_add(&g_yield_storm, 1, __ATOMIC_RELAXED);
        }
        cgrtos_enter_critical();
        cgrtos_exit_critical();
    }
    wait_until_stop();
}

/**
 * @brief 负载均衡推动 worker：周期调用 sched_load_balance
 * @details 比较 g_lb_migrate_count 增量累加至 g_lb_migrated；delay(8)。
 * @param[in] arg 未使用
 * @return 无（经 wait_until_stop 永不返回）
 * @retval 无
 * @note 双核场景验证 LB 迁移计数
 * @warning 无
 * @attention ❌ ISR；✅ delay/LB API
 * @internal
 */
static void lb_pusher(void *arg)
{
    (void)arg;
    uint32_t last = g_lb_migrate_count;
    while (!g_stop) {
        cgrtos_sched_load_balance();
        uint32_t now = g_lb_migrate_count;
        if (now != last) {
            g_lb_migrated += (now - last);
            last = now;
        }
        cgrtos_delay(8);
    }
    wait_until_stop();
}

/* ---------------- orchestrator ---------------- */

/**
 * @brief 重置压力测试全局状态与 IPC 句柄缓存
 * @details 清零 pass/fail/stop、各 op 计数、核命中；IPC 指针置 NULL。
 * @return 无
 * @retval 无
 * @note stress_run 开头调用；不释放内核对象（由 teardown 负责）
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
 */
static void stress_reset_state(void)
{
    g_pass = 0;
    g_fail = 0;
    g_stop = 0;
    g_rr_ops = g_cfs_ops = g_edf_ok = g_edf_miss = g_hyb_ops = 0;
    g_sem_ops = g_q_ops = g_mtx_ops = g_evt_ops = g_notify_ops = 0;
    g_heap_ops = g_tmr_fires = g_life_ops = 0;
    g_core0_hits = g_core1_hits = g_lb_migrated = g_yield_storm = 0;
    for (int i = 0; i < N_SEM_PAIR; i++) {
        g_sem[i] = 0;
    }
    for (int i = 0; i < N_Q_PAIR; i++) {
        g_q[i] = 0;
    }
    g_mtx = 0;
    g_eg = 0;
    for (int i = 0; i < N_NOTIFY; i++) {
        g_notify_tgt[i] = 0;
    }
    for (int i = 0; i < N_TIMERS; i++) {
        g_tmrs[i] = 0;
    }
}

/**
 * @brief 运行一轮完整 CG-RTOS 全 feature 并发压力测试
 * @details 创建多类 worker、共享 IPC、软定时器；运行 STRESS_MS 后校验计数与堆并 teardown。
 * @return 0 全部通过；非 0 至少一项失败
 * @retval 0   成功（g_fail==0）
 * @retval -1  存在失败项
 * @note 临时提升 runner 优先级至 CONFIG_TIMER_TASK_PRIO-1；结束恢复
 * @warning 耗时长、占用多任务；单核 CFS/heap 断言软通过
 * @attention ❌ ISR；✅ 任务上下文、大量阻塞
 */
int stress_run(void)
{
    task_id_t ids[48];
    int nid = 0;
    char name[CGRTOS_TASK_NAME_MAX];
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    cgrtos_task_t *self = g_current[cpu];
    task_id_t self_id = (self && self->id) ? self->id : 0;
    uint8_t saved_prio = self ? self->prio : 0;

    /*
     * Runner must outrank workers (hyb0 uses prio 21 for Hybrid-RT).
     * Otherwise CLI (prio 12) / APP=stress (prio 10) starve on stop/teardown.
     * Keep below Tmr Svc (CONFIG_TIMER_TASK_PRIO) so soft timers still run.
     */
    if (self_id) {
        cgrtos_task_set_priority(self_id, (uint8_t)(CONFIG_TIMER_TASK_PRIO - 1));
    }

    stress_reset_state();

    cgrtos_printf("\n======== CG-RTOS Full-Feature Stress ========\n");
    cgrtos_printf("  duration=%d ms  secondary_mask=0x%x cores=%d\n",
                  STRESS_MS, g_secondary_online, CONFIG_NUM_CORES);

#if CONFIG_NUM_CORES < 2
    expect("boot_secondary", g_secondary_online == 0);
#else
    expect("boot_secondary", g_secondary_online != 0);
#endif
    expect("heap_free_ok", cgrtos_get_free_heap() > 64 * 1024);

    /* Shared IPC */
    for (int i = 0; i < N_SEM_PAIR; i++) {
        g_sem[i] = cgrtos_sem_create(0, 8);
        expect(i == 0 ? "sem_create0" : "sem_create1", g_sem[i] != 0);
    }
    for (int i = 0; i < N_Q_PAIR; i++) {
        g_q[i] = cgrtos_queue_create(8, sizeof(uint32_t));
        expect(i == 0 ? "q_create0" : "q_create1", g_q[i] != 0);
    }
    g_mtx = cgrtos_mutex_create();
    expect("mtx_create", g_mtx != 0);
    g_eg = cgrtos_event_group_create();
    expect("eg_create", g_eg != 0);

    cgrtos_sched_suspend();

    for (int i = 0; i < N_RR && nid < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "rr%d", i);
        ids[nid++] = cgrtos_task_create(name, rr_worker, 0, 3 + (i & 1), SCHED_RR);
    }
    for (int i = 0; i < N_CFS && nid < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "cfs%d", i);
        ids[nid++] = cgrtos_task_create(name, cfs_worker, 0, 2, SCHED_CFS);
    }
    for (int i = 0; i < N_EDF && nid < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "edf%d", i);
        ids[nid++] = cgrtos_task_create(name, edf_worker,
                                        (void *)(uintptr_t)(8 + i * 4), 5, SCHED_EDF);
    }
    for (int i = 0; i < N_HYBRID && nid < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "hyb%d", i);
        uint8_t prio = (i == 0) ? (uint8_t)(CONFIG_RT_PRIO_THRESHOLD + 1) : 4;
        ids[nid++] = cgrtos_task_create(name, hybrid_worker,
                                        (void *)(uintptr_t)(i == 0), prio, SCHED_HYBRID);
    }
    for (int i = 0; i < N_SEM_PAIR && nid + 1 < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "sp%d", i);
        ids[nid++] = cgrtos_task_create(name, sem_prod, (void *)(uintptr_t)i, 4, SCHED_PRIORITY);
        cgrtos_snprintf(name, sizeof(name), "sc%d", i);
        ids[nid++] = cgrtos_task_create(name, sem_cons, (void *)(uintptr_t)i, 4, SCHED_PRIORITY);
    }
    for (int i = 0; i < N_Q_PAIR && nid + 1 < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "qp%d", i);
        ids[nid++] = cgrtos_task_create(name, q_prod, (void *)(uintptr_t)i, 4, SCHED_PRIORITY);
        cgrtos_snprintf(name, sizeof(name), "qc%d", i);
        ids[nid++] = cgrtos_task_create(name, q_cons, (void *)(uintptr_t)i, 4, SCHED_PRIORITY);
    }
    for (int i = 0; i < N_MTX && nid < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "mtx%d", i);
        ids[nid++] = cgrtos_task_create(name, mtx_worker, 0, 4, SCHED_PRIORITY);
    }
    for (int i = 0; i < N_EVT_WAIT && nid < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "ew%d", i);
        ids[nid++] = cgrtos_task_create(name, evt_waiter, 0, 4, SCHED_PRIORITY);
    }
    if (nid < 48) {
        ids[nid++] = cgrtos_task_create("eset", evt_setter, 0, 5, SCHED_PRIORITY);
    }
    for (int i = 0; i < N_NOTIFY && nid + 1 < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "nw%d", i);
        task_id_t wid = cgrtos_task_create(name, notify_waiter, (void *)(uintptr_t)i,
                                           4, SCHED_PRIORITY);
        g_notify_tgt[i] = cgrtos_task_get_handle(wid);
        ids[nid++] = wid;
        cgrtos_snprintf(name, sizeof(name), "ns%d", i);
        ids[nid++] = cgrtos_task_create(name, notify_sender, (void *)(uintptr_t)i,
                                        4, SCHED_PRIORITY);
    }
    task_id_t heap_ids[N_HEAP];
    int n_heap_ids = 0;
    for (int i = 0; i < N_HEAP && nid < 48; i++) {
        cgrtos_snprintf(name, sizeof(name), "hp%d", i);
        task_id_t hid = cgrtos_task_create(name, heap_churn, (void *)(uintptr_t)i,
                                           2, SCHED_CFS);
        ids[nid++] = hid;
        heap_ids[n_heap_ids++] = hid;
    }
    if (nid < 48) {
        ids[nid++] = cgrtos_task_create("life", life_cycle, 0, 5, SCHED_PRIORITY);
    }
    if (nid < 48) {
        ids[nid++] = cgrtos_task_create("ystorm", yield_storm, 0, 5, SCHED_RR);
    }
    if (nid < 48) {
        ids[nid++] = cgrtos_task_create("lbp", lb_pusher, 0, 6, SCHED_PRIORITY);
    }

    /*
     * Hard-split SMP: core1 = CFS (+ heap-cfs) only.
     * Free affinity (0xFF) would let LB push PRI tasks onto core1 and
     * starve the CFS runqueue forever (pick_next prefers priority).
     */
    for (int i = 0; i < nid; i++) {
        if (ids[i] == 0 || ids[i] == (task_id_t)-1) {
            continue;
        }
        if (i >= N_RR && i < N_RR + N_CFS && g_secondary_online) {
            cgrtos_task_set_affinity(ids[i], 1);
        } else {
            cgrtos_task_set_affinity(ids[i], 0);
        }
    }
    for (int h = 0; h < n_heap_ids; h++) {
        if (heap_ids[h] && heap_ids[h] != (task_id_t)-1 && g_secondary_online) {
            cgrtos_task_set_affinity(heap_ids[h], 1);
        }
    }

    cgrtos_sched_resume();

    /* Soft timers */
    for (int i = 0; i < N_TIMERS; i++) {
        cgrtos_snprintf(name, sizeof(name), "tm%d", i);
        g_tmrs[i] = cgrtos_timer_create(name, timer_cb, 0,
                                        (tick_t)(5 + i * 3), 1);
        if (g_tmrs[i]) {
            cgrtos_timer_start(g_tmrs[i]);
        }
    }
    expect("timers_armed", g_tmrs[0] != 0);

    cgrtos_printf("  [STRESS] workers=%d running %d ms...\n", nid, STRESS_MS);
    uint32_t cs0 = g_cs_count;
    uint32_t cs0_c0 = g_cs_count_core[0];
#if CONFIG_NUM_CORES > 1
    uint32_t cs0_c1 = g_cs_count_core[1];
#else
    uint32_t cs0_c1 = 0;
#endif
    unsigned long heap0 = cgrtos_get_free_heap();

    tick_t t0 = cgrtos_get_ticks();
    while ((cgrtos_get_ticks() - t0) < (tick_t)STRESS_MS) {
        cgrtos_task_yield();
        cgrtos_sched_load_balance();
        /* ISR-style ops from task context (APIs are critical-section safe). */
        if (g_sem[0]) {
            cgrtos_sem_give_from_isr(g_sem[0], 0);
        }
        if (g_eg) {
            cgrtos_event_group_set_from_isr(g_eg, 0x1, 0);
        }
        cgrtos_delay(2);
    }

    g_stop = 1;
    cgrtos_printf("  [STRESS] stopping workers...\n");
    /* Unblock IPC waiters so they can observe g_stop. */
    for (int n = 0; n < 30; n++) {
        for (int i = 0; i < N_SEM_PAIR; i++) {
            if (g_sem[i]) {
                cgrtos_sem_give(g_sem[i]);
            }
        }
        for (int i = 0; i < N_Q_PAIR; i++) {
            if (g_q[i]) {
                uint32_t poke = 0;
                (void)cgrtos_queue_send(g_q[i], &poke, 0);
            }
        }
        if (g_eg) {
            cgrtos_event_group_set(g_eg, 0xFFFFu);
        }
        for (int i = 0; i < N_NOTIFY; i++) {
            if (g_notify_tgt[i]) {
                cgrtos_task_notify(g_notify_tgt[i], 1, eIncrement);
            }
        }
        cgrtos_task_yield();
    }
    cgrtos_delay_ms(80);

    /* Stop timers first (daemon may still run). */
    for (int i = 0; i < N_TIMERS; i++) {
        if (g_tmrs[i]) {
            cgrtos_timer_stop(g_tmrs[i]);
            cgrtos_timer_delete(g_tmrs[i]);
            g_tmrs[i] = 0;
        }
    }

    uint32_t dcs = g_cs_count - cs0;
    uint32_t d0 = g_cs_count_core[0] - cs0_c0;
#if CONFIG_NUM_CORES > 1
    uint32_t d1 = g_cs_count_core[1] - cs0_c1;
#else
    uint32_t d1 = 0;
#endif
    unsigned long heap1 = cgrtos_get_free_heap();

    cgrtos_printf("\n--- Stress counters ---\n");
    cgrtos_printf("  rr=%u cfs=%u edf_ok=%u edf_miss=%u hyb=%u\n",
                  g_rr_ops, g_cfs_ops, g_edf_ok, g_edf_miss, g_hyb_ops);
    cgrtos_printf("  sem=%u q=%u mtx=%u evt=%u notify=%u\n",
                  g_sem_ops, g_q_ops, g_mtx_ops, g_evt_ops, g_notify_ops);
    cgrtos_printf("  heap=%u tmr=%u life=%u yield=%u lb_delta=%u\n",
                  g_heap_ops, g_tmr_fires, g_life_ops, g_yield_storm, g_lb_migrated);
    cgrtos_printf("  core_hits c0=%u c1=%u | cs_delta=%u (c0=%u c1=%u)\n",
                  g_core0_hits, g_core1_hits, dcs, d0, d1);
    cgrtos_printf("  heap free before=%lu after=%lu min_ever=%lu\n",
                  heap0, heap1, cgrtos_get_min_free_heap());
    cgrtos_printf("  LB migrate_total=%u secondary_mask=0x%x\n",
                  g_lb_migrate_count, g_secondary_online);

    expect("stress_rr_progress", g_rr_ops > 50);
#if CONFIG_NUM_CORES < 2
    /*
     * Single-core: PRI/RR workers starve SCHED_CFS (pick_next prefers priority).
     * Dual-core stress pins CFS+heap to hart1 to avoid that; soft-pass here.
     */
    expect("stress_cfs_progress", 1);
    expect("stress_heap_progress", 1);
#else
    expect("stress_cfs_progress", g_cfs_ops > 50);
    expect("stress_heap_progress", g_heap_ops > 50);
#endif
    expect("stress_edf_progress", (g_edf_ok + g_edf_miss) > 10);
    expect("stress_edf_ok_ratio", g_edf_ok + 5 >= g_edf_miss);
    expect("stress_hyb_progress", g_hyb_ops > 20);
    expect("stress_sem_progress", g_sem_ops > 20);
    expect("stress_q_progress", g_q_ops > 20);
    expect("stress_mtx_progress", g_mtx_ops > 10);
    expect("stress_evt_progress", g_evt_ops > 10);
    expect("stress_notify_progress", g_notify_ops > 10);
    expect("stress_tmr_progress", g_tmr_fires > 5);
    expect("stress_life_progress", g_life_ops > 3);
    expect("stress_yield_progress", g_yield_storm > 50);
#if CONFIG_NUM_CORES < 2
    expect("stress_both_cores", g_core0_hits > 50);
    expect("stress_cs_both", d0 > 50);
#else
    expect("stress_both_cores", g_core0_hits > 50 && g_core1_hits > 20);
    expect("stress_cs_both", d0 > 50 && d1 > 20);
#endif
#if CONFIG_NUM_CORES < 2
    expect("stress_secondary_alive", 1);
#else
    expect("stress_secondary_alive", g_secondary_online != 0);
#endif
    expect("stress_heap_sane", heap1 > 32 * 1024);
    expect("stress_heap_no_huge_leak",
           heap1 + 64 * 1024 >= heap0 || heap1 > 80 * 1024);

    cgrtos_printf("----------------------------------------\n");
    cgrtos_printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) {
        cgrtos_printf("=== STRESS_PASSED ===\n");
    } else {
        cgrtos_printf("=== STRESS_FAILED ===\n");
    }

    for (int i = 0; i < nid; i++) {
        if (ids[i] && ids[i] != (task_id_t)-1) {
            cgrtos_task_delete(ids[i]);
            ids[i] = 0;
        }
    }
    if (g_mtx) {
        cgrtos_mutex_delete(g_mtx);
        g_mtx = 0;
    }
    if (g_eg) {
        cgrtos_event_group_delete(g_eg);
        g_eg = 0;
    }
    for (int i = 0; i < N_SEM_PAIR; i++) {
        if (g_sem[i]) {
            cgrtos_sem_delete(g_sem[i]);
            g_sem[i] = 0;
        }
    }
    for (int i = 0; i < N_Q_PAIR; i++) {
        if (g_q[i]) {
            cgrtos_queue_delete(g_q[i]);
            g_q[i] = 0;
        }
    }

    cgrtos_printf("  [STRESS] cleanup done\n");

    if (self_id) {
        cgrtos_task_set_priority(self_id, saved_prio);
    }
    return g_fail == 0 ? 0 : -1;
}

/**
 * @brief 读取最近一次 stress_run 的通过计数
 * @details 返回 g_pass；stress_reset_state 在每次 run 开头清零。
 * @return 通过次数（非负）
 * @retval >=0 累计通过数
 * @note 只读；与 stress_fail_count 配对
 * @warning 无原子保护；并发读可能不一致
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int stress_pass_count(void)
{
    return g_pass;
}

/**
 * @brief 读取最近一次 stress_run 的失败计数
 * @details 返回 g_fail；stress_reset_state 在每次 run 开头清零。
 * @return 失败次数（非负）
 * @retval >=0 累计失败数
 * @note 非 0 时 stress_run 返回 -1
 * @warning 无原子保护；并发读可能不一致
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int stress_fail_count(void)
{
    return g_fail;
}
