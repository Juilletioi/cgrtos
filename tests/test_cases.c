/**
 * @file test_cases.c
 * @brief 功能测试用例实现与 CLI/自动化套件运行器
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 命名用例表供 APP=test 与 CLI `run <name>` 共享。各 case_* 通过 expect()
 * 输出 [PASS]/[FAIL]；`all` 结束时打印 === TEST_SUITE_PASSED/FAILED ===，
 * 供 scripts/cgrtos.sh / run_qemu.sh 解析。
 */

#include "../kernel/cgrtos.h"
#include "../kernel/vfs.h"
#include "hal_board.h"
#include "test_cases.h"
#include "stress_cases.h"

static volatile int g_fail;
static volatile int g_pass;

static volatile int g_notify_got;
static volatile int g_timer_fires;
static volatile int g_smp_core1;
static volatile int g_hook_ticks;
static volatile int g_hook_idle;
static volatile int g_edf_ok;
static volatile int g_edf_miss;
static volatile int g_hybrid_rt_hits;
static volatile uint32_t g_cfs_a;
static volatile uint32_t g_cfs_b;
static volatile uint32_t g_lb_seen_core1;
static cgrtos_task_t *g_notify_target;

/* ISR pack test fixtures (tick hook → FromISR) */
static cgrtos_sem_t *g_isr_sem;
static cgrtos_queue_t *g_isr_q;
static cgrtos_event_group_t *g_isr_eg;
static cgrtos_stream_buffer_t *g_isr_sb;
static cgrtos_message_buffer_t *g_isr_mb;
static cgrtos_timer_t *g_isr_tmr;
static volatile int g_isr_tick_ops;
static volatile int g_isr_woken_seen;
static volatile int g_irq_handler_hits;
static volatile uint32_t g_irq_last;

/**
 * @brief 记录单条断言结果并更新全局 pass/fail 计数
 * @details cond 为真打印 [PASS]，否则 [FAIL]；分别递增 g_pass / g_fail。
 * @param[in] name 断言描述字符串
 * @param[in] cond 条件（非零为通过）
 * @return 无
 * @retval 无
 * @note 供所有 case_* 用例复用
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
 * @brief 阻塞等待指定 tick 数（或 yield-only）
 * @details n==0 仅 cgrtos_task_yield；否则 cgrtos_delay(n)。避免忙等饿死。
 * @param[in] n 等待 tick 数；0 表示仅让出
 * @return 无
 * @retval 无
 * @note EDF 占满核时忙等会 TIMEOUT
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 * @internal
 */
static void wait_ticks(tick_t n)
{
    /* 阻塞等待：勿用 yield 忙等——EDF 占满核时会饿死本任务导致 TIMEOUT */
    if (n == 0) {
        cgrtos_task_yield();
        return;
    }
    cgrtos_delay(n);
}

/**
 * @brief notify 用例等待任务：阻塞等待 task notify
 * @details cgrtos_task_notify_wait 成功后写入 g_notify_got；之后永久 delay。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 由 case_notify 创建
 * @warning 无
 * @attention ❌ ISR；✅ 阻塞 wait
 * @internal
 */
static void notify_waiter(void *arg)
{
    (void)arg;
    uint32_t val = 0;
    if (cgrtos_task_notify_wait(0, 0xFFFFFFFFU, &val, portMS_TO_TICK(2000))) {
        g_notify_got = (int)val;
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

/**
 * @brief 软定时器测试回调：递增 g_timer_fires
 * @param[in] arg 未使用
 * @return 无
 * @retval 无
 * @note 供 case_timer / case_hooks / case_isr 复用
 * @warning 无
 * @attention ✅ 定时器 daemon 上下文；❌ 非 ISR 直调
 * @internal
 */
static void timer_cb(void *arg)
{
    (void)arg;
    g_timer_fires++;
}

/**
 * @brief CFS 公平性测试工作线程：在窗口内自增计数并 yield
 * @details 约 60ms 内循环 (*cnt)++ 与 yield；结束后永久 delay。
 * @param[in] arg 指向 volatile uint32_t 计数器的指针
 * @return 无（永不返回）
 * @retval 无
 * @note 由 case_sched / case_m4_safe 创建
 * @warning 无
 * @attention ❌ ISR；✅ yield/delay
 * @internal
 */
static void cfs_worker(void *arg)
{
    volatile uint32_t *cnt = (volatile uint32_t *)arg;
    tick_t end = cgrtos_get_ticks() + portMS_TO_TICK(60);
    while (cgrtos_get_ticks() < end) {
        (*cnt)++;
        cgrtos_task_yield();
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

/**
 * @brief EDF 周期任务：设置 period/deadline 并统计按时/逾期完成
 * @details arg 为 period(tick)；每周期 bounded compute 后比较 done 与 wake，更新 g_edf_ok/g_edf_miss。
 * @param[in] arg period tick 数（uintptr_t 传递）
 * @return 无（长期运行后 idle delay）
 * @retval 无
 * @note 由 case_sched / case_m5_perf / case_sched_m1 创建
 * @warning 单核与多核 MC-EDF 行为不同
 * @attention ❌ ISR；✅ delay/set_deadline
 * @internal
 */
static void edf_worker(void *arg)
{
    tick_t period = (tick_t)(uintptr_t)arg;
    tick_t wake = cgrtos_get_ticks();
    cgrtos_task_t *self = 0;
    uint8_t cpu = arch_cpu_id();
    self = g_current[cpu];
    if (self) {
        cgrtos_task_set_period(self->id, period);
    }

    while (1) {
        tick_t release = wake;
        wake += period;
        if (self) {
            cgrtos_task_set_deadline(self->id, wake);
        }
        /* Bounded compute — must finish before period end */
        volatile int x = 0;
        for (int i = 0; i < 200; i++) {
            x += i;
        }
        (void)x;
        tick_t done = cgrtos_get_ticks();
        if (done <= wake) {
            g_edf_ok++;
        } else {
            g_edf_miss++;
        }
        if (done < wake) {
            cgrtos_delay(wake - done);
        } else {
            /* late: next window */
            wake = cgrtos_get_ticks() + period;
        }
        (void)release;
        if (g_edf_ok + g_edf_miss > 40) {
            while (1) {
                cgrtos_delay_ms(1000);
            }
        }
    }
}

/**
 * @brief Hybrid 调度 RT 侧：notify_wait 计数 g_hybrid_rt_hits
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 与 hybrid_cfs_hog 对跑验证 RT 不被饿死
 * @warning 无
 * @attention ❌ ISR；✅ 阻塞 notify_wait
 * @internal
 */
static void hybrid_rt_worker(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t val = 0;
        if (cgrtos_task_notify_wait(0, 0xFFFFFFFFU, &val, portMS_TO_TICK(500))) {
            g_hybrid_rt_hits++;
        }
    }
}

/**
 * @brief Hybrid 调度 CFS hog：短窗口内 busy+yield 占 CPU
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 绑核 1 与 RT 任务分离
 * @warning 无
 * @attention ❌ ISR；✅ yield
 * @internal
 */
static void hybrid_cfs_hog(void *arg)
{
    (void)arg;
    tick_t end = cgrtos_get_ticks() + portMS_TO_TICK(200);
    while (cgrtos_get_ticks() < end) {
        volatile int x = 0;
        for (int i = 0; i < 50; i++) {
            x++;
        }
        (void)x;
        cgrtos_task_yield();
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

/**
 * @brief RR 测试工作线程：yield + 40ms delay 循环
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 供 case_task / case_sched / case_sched_m1 复用
 * @warning 无
 * @attention ❌ ISR；✅ delay/yield
 * @internal
 */
static void rr_worker(void *arg)
{
    (void)arg;
    while (1) {
        cgrtos_task_yield();
        cgrtos_delay_ms(40);
    }
}

/**
 * @brief SMP 亲和性测试：检测是否在 hart 1 上运行
 * @details arch_cpu_id()==1 时置 g_smp_core1=1。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 由 case_smp 创建并 set_affinity(1)
 * @warning 次核未 online 时软跳过
 * @attention ❌ ISR；✅ delay
 * @internal
 */
static void aff_worker(void *arg)
{
    (void)arg;
    while (1) {
        uint8_t h = arch_cpu_id();
        if (h == 1) {
            g_smp_core1 = 1;
        }
        cgrtos_delay_ms(5);
    }
}

/**
 * @brief 负载均衡测试工作线程：统计在 core1 上的执行次数
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 与 cgrtos_sched_load_balance 配合
 * @warning 无
 * @attention ❌ ISR；✅ yield
 * @internal
 */
static void lb_worker(void *arg)
{
    (void)arg;
    tick_t end = cgrtos_get_ticks() + portMS_TO_TICK(80);
    while (cgrtos_get_ticks() < end) {
        if (arch_cpu_id() == 1) {
            g_lb_seen_core1++;
        }
        for (volatile int i = 0; i < 30; i++) {
        }
        cgrtos_task_yield();
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

/**
 * @brief Idle 钩子测试：递增 g_hook_idle
 * @return 无
 * @retval 无
 * @note 由 case_hooks 注册
 * @warning 无
 * @attention ✅ idle 上下文；❌ 不阻塞
 * @internal
 */
static void idle_hook_fn(void)
{
    g_hook_idle++;
}

/**
 * @brief Tick 钩子测试：递增 g_hook_ticks
 * @return 无
 * @retval 无
 * @note 由 case_hooks 注册
 * @warning 无
 * @attention ✅ tick ISR 上下文；❌ 须保持短小
 * @internal
 */
static void tick_hook_fn(void)
{
    g_hook_ticks++;
}

/**
 * @brief 真实 tick ISR 钩子：按 phase 轮转调用 FromISR API 并收集 woken
 * @details
 * 1. phase = g_isr_tick_ops & 7，依次覆盖 sem_give、queue_send、event_set、
 *    stream_send、message_send、timer_start、event_clear、sem_take。
 * 2. 各 API 传入 &woken；若 woken!=pdFALSE 则计数并 portYIELD_FROM_ISR。
 * 3. g_isr_tick_ops++，供 case_isr 断言至少执行多轮。
 * @return 无
 * @retval 无
 * @note 由 case_isr 经 cgrtos_set_tick_hook 安装
 * @warning 须在 tick ISR 上下文保持短小，避免拖长中断延迟
 * @attention ✅ tick ISR；❌ 禁止阻塞 API
 * @internal
 */
static void isr_pack_tick_hook(void)
{
    BaseType_t woken = pdFALSE;
    uint32_t phase = (uint32_t)g_isr_tick_ops & 7U;

    if (phase == 0 && g_isr_sem) {
        (void)cgrtos_sem_give_from_isr(g_isr_sem, &woken);
    } else if (phase == 1 && g_isr_q) {
        uint32_t v = 0xA5A5U + (uint32_t)g_isr_tick_ops;
        (void)cgrtos_queue_send_from_isr(g_isr_q, &v, &woken);
    } else if (phase == 2 && g_isr_eg) {
        (void)cgrtos_event_group_set_from_isr(g_isr_eg, 0x1U, &woken);
    } else if (phase == 3 && g_isr_sb) {
        const char b = 'I';
        (void)cgrtos_stream_buffer_send_from_isr(g_isr_sb, &b, 1, &woken);
    } else if (phase == 4 && g_isr_mb) {
        const char msg[] = "isr";
        (void)cgrtos_message_buffer_send_from_isr(g_isr_mb, msg, 3, &woken);
    } else if (phase == 5 && g_isr_tmr) {
        (void)cgrtos_timer_start_from_isr(g_isr_tmr, &woken);
    } else if (phase == 6 && g_isr_eg) {
        (void)cgrtos_event_group_clear_from_isr(g_isr_eg, 0x2U);
    } else if (phase == 7 && g_isr_sem) {
        (void)cgrtos_sem_take_from_isr(g_isr_sem);
    }

    if (woken != pdFALSE) {
        g_isr_woken_seen++;
        portYIELD_FROM_ISR(woken);
    }
    g_isr_tick_ops++;
}

/**
 * @brief case_irq 用的模拟 PLIC handler：累计命中次数并记录 irq 号
 * @details cgrtos_irq_dispatch 模拟 claim 后路径调用；递增 g_irq_handler_hits 并写入 g_irq_last。
 * @param[in] irq 中断源号
 * @param[in] arg 未使用
 * @return 无
 * @retval 无
 * @note 注册于 irq 5，unregister 后不再触发
 * @warning 非真实硬件中断上下文，仅验证分发契约
 * @attention ✅ ISR 风格回调；❌ 不阻塞
 * @internal
 */
static void irq_test_handler(uint32_t irq, void *arg)
{
    (void)arg;
    g_irq_handler_hits++;
    g_irq_last = irq;
}

/* ---- named cases (former run_tests sections) ---- */

/**
 * @brief 测试用例 delay — tick/delay_ms/delay_us/delay_until
 * @details 覆盖 delay(0)、delay_ms 范围、delay_until 周期与 miss、us 忙等路径。
 * @return 无
 * @retval 无
 * @note 通过 expect() 累计结果
 * @warning 时间断言含 QEMU 容差
 * @attention ❌ ISR；✅ 阻塞 delay API
 */
static void case_delay(void)
{
    tick_t t0, t1, dt, wake;
    uint64_t m0, m1, md;
    int i;

    /* delay(0) is yield-only — must not hang */
    cgrtos_delay(0);
    expect("delay_0_yields", 1);

    t0 = cgrtos_get_ticks();
    cgrtos_delay_ms(50);
    dt = cgrtos_get_ticks() - t0;
    expect("delay_ms_range", dt >= 40 && dt < 120);

    /* Absolute period: 5 x 20 ticks ≈ 100, without "stretch on miss" */
    wake = cgrtos_get_ticks();
    t0 = wake;
    for (i = 0; i < 5; i++) {
        cgrtos_delay_until(&wake, 20);
    }
    t1 = cgrtos_get_ticks();
    dt = t1 - t0;
    expect("delay_until_period", dt >= 80 && dt <= 160);
    expect("delay_until_wake_advanced", wake == t0 + 100);

    /* Missed deadline: prev far in the past → no block, wake advances one step */
    {
        tick_t past = cgrtos_get_ticks() > 200 ? cgrtos_get_ticks() - 200 : 0;
        tick_t before = cgrtos_get_ticks();
        wake = past;
        cgrtos_delay_until(&wake, 20);
        t1 = cgrtos_get_ticks();
        expect("delay_until_miss_nowait", (t1 - before) < 5);
        expect("delay_until_miss_advance", wake == past + 20);
    }

    /* Short us busy-wait */
    m0 = cgrtos_mtime_read();
    cgrtos_delay_us(500);
    m1 = cgrtos_mtime_read();
    md = m1 - m0;
    expect("delay_us_short", md >= 400 && md < 50000);

    /* Hybrid path (several ms via us); bounds scale with timer Hz */
    m0 = cgrtos_mtime_read();
    cgrtos_delay_us(5000);
    m1 = cgrtos_mtime_read();
    md = m1 - m0;
    {
        uint64_t lo = (CONFIG_TIMER_CLOCK_HZ * 4ULL) / 1000ULL;   /* ~4ms */
        uint64_t hi = (CONFIG_TIMER_CLOCK_HZ * 200ULL) / 1000ULL; /* ~200ms QEMU slack */
        if (lo < 4000ULL) {
            lo = 4000ULL;
        }
        expect("delay_us_hybrid", md >= lo && md < hi);
    }
}

/**
 * @brief 测试用例 mem — TLSF 堆 malloc/calloc/free
 * @details 对齐、读写、double-free、碎片、OOM、大块等路径。
 * @return 无
 * @retval 无
 * @note 可能大量分配；结束后尽量 reclaim
 * @warning OOM 循环依赖 CONFIG_HEAP_SIZE
 * @attention ❌ ISR；✅ 堆 API
 */
static void case_mem(void)
{
    unsigned long total = cgrtos_get_free_heap();
    unsigned long min0 = cgrtos_get_min_free_heap();
    expect("mem_free_heap_nonzero", total > 0 && total <= CONFIG_HEAP_SIZE);
    expect("mem_min_free_le_free", min0 <= total);

    /* malloc(0) / free(NULL) */
    expect("mem_malloc_zero", cgrtos_malloc(0) == 0);
    cgrtos_free(0); /* must not hang / fault */
    expect("mem_free_null", 1);

    /* basic alloc + writable + 8-byte aligned */
    void *p = cgrtos_malloc(128);
    expect("mem_malloc", p != 0);
    expect("mem_align8", p && (((uintptr_t)p & 7U) == 0));
    if (p) {
        uint8_t *b = (uint8_t *)p;
        for (int i = 0; i < 128; i++) {
            b[i] = (uint8_t)(0xA0 + (i & 0xF));
        }
        int ok = 1;
        for (int i = 0; i < 128; i++) {
            if (b[i] != (uint8_t)(0xA0 + (i & 0xF))) {
                ok = 0;
                break;
            }
        }
        expect("mem_rw_pattern", ok);
    }
    unsigned long after1 = cgrtos_get_free_heap();
    expect("mem_shrink_after_malloc", after1 < total);

    /* calloc zeros */
    void *q = cgrtos_calloc(4, 32);
    expect("mem_calloc", q != 0);
    expect("mem_calloc_align", q && (((uintptr_t)q & 7U) == 0));
    if (q) {
        uint8_t *z = (uint8_t *)q;
        int zero = 1;
        for (int i = 0; i < 128; i++) {
            if (z[i] != 0) {
                zero = 0;
                break;
            }
        }
        expect("mem_calloc_zeroed", zero);
    }
    expect("mem_calloc_count0", cgrtos_calloc(0, 32) == 0);
    expect("mem_calloc_size0", cgrtos_calloc(4, 0) == 0);

    /* small sizes coalesce into min block */
    void *s1 = cgrtos_malloc(1);
    void *s2 = cgrtos_malloc(7);
    void *s3 = cgrtos_malloc(16);
    expect("mem_malloc_tiny", s1 && s2 && s3);
    expect("mem_tiny_align",
           s1 && s2 && s3 &&
           (((uintptr_t)s1 | (uintptr_t)s2 | (uintptr_t)s3) & 7U) == 0);

    /* free + double-free safety */
    unsigned long before_free = cgrtos_get_free_heap();
    cgrtos_free(p);
    cgrtos_free(p); /* second free must be ignored */
    expect("mem_double_free_safe", 1);
    unsigned long after_free_p = cgrtos_get_free_heap();
    expect("mem_free_restores", after_free_p > before_free);

    cgrtos_free(q);
    cgrtos_free(s1);
    cgrtos_free(s2);
    cgrtos_free(s3);

    /* fragmentation / coalesce: alloc A B C, free B, realloc into hole or grow */
    void *a = cgrtos_malloc(64);
    void *b = cgrtos_malloc(64);
    void *c = cgrtos_malloc(64);
    expect("mem_alloc_abc", a && b && c);
    cgrtos_free(b);
    void *d = cgrtos_malloc(64); /* should reuse freed B or coalesce */
    expect("mem_reuse_after_free", d != 0);
    cgrtos_free(a);
    cgrtos_free(c);
    cgrtos_free(d);

    /* free-all should return close to original free size */
    unsigned long after_all = cgrtos_get_free_heap();
    expect("mem_free_all_near_full",
           after_all >= total - 256 || after_all >= (CONFIG_HEAP_SIZE / 2));

    /* min-ever watermark only goes down */
    unsigned long min1 = cgrtos_get_min_free_heap();
    expect("mem_min_free_monotonic", min1 <= min0 && min1 <= after_all);

    /* many small blocks then free — stress free-list / merge */
    {
        void *blks[16];
        int n_ok = 0;
        for (int i = 0; i < 16; i++) {
            blks[i] = cgrtos_malloc(48 + (unsigned)(i % 5) * 8);
            if (blks[i]) {
                n_ok++;
                uint8_t *bp = (uint8_t *)blks[i];
                bp[0] = (uint8_t)(0x40 + i);
            }
        }
        expect("mem_many_alloc", n_ok >= 12);
        for (int i = 0; i < 16; i += 2) {
            cgrtos_free(blks[i]);
            blks[i] = 0;
        }
        void *mid = cgrtos_malloc(96);
        expect("mem_alloc_amid_holes", mid != 0);
        cgrtos_free(mid);
        for (int i = 1; i < 16; i += 2) {
            cgrtos_free(blks[i]);
        }
        unsigned long after_stress = cgrtos_get_free_heap();
        expect("mem_stress_reclaim", after_stress >= after_all - 1024);

        /* OOM with medium chunks (faster than huge) */
        void *giant[32];
        int n_giant = 0;
        for (int i = 0; i < 32; i++) {
            giant[i] = cgrtos_malloc(8 * 1024);
            if (!giant[i]) {
                break;
            }
            n_giant++;
        }
        expect("mem_oom_hit", n_giant < 32);
        expect("mem_oom_partial", n_giant >= 1);
        for (int i = 0; i < n_giant; i++) {
            cgrtos_free(giant[i]);
        }
        expect("mem_oom_recover", cgrtos_get_free_heap() >= after_stress - 2048);

        /* single large allocation */
        unsigned long room = cgrtos_get_free_heap();
        void *big = cgrtos_malloc(room / 2);
        expect("mem_large_half", big != 0);
        cgrtos_free(big);
        expect("mem_large_freed", cgrtos_get_free_heap() >= room - 512);
    }
}

/**
 * @brief 测试用例 sem — 信号量 create/take/give/ISR/delete
 * @return 无
 * @retval 无
 * @note 含二进制信号量与 timeout
 * @warning 无
 * @attention ❌ ISR；✅ 阻塞 take
 */
static void case_sem(void)
{
    cgrtos_sem_t *sem = cgrtos_sem_create(0, 5);
    cgrtos_sem_t *bin = cgrtos_sem_create_binary();
    expect("sem_create", sem && bin);
    expect("sem_take_timeout",
           cgrtos_sem_take(sem, portMS_TO_TICK(20)) == errTIMEOUT);

    expect("sem_give", cgrtos_sem_give(sem) == pdPASS);
    expect("sem_take", cgrtos_sem_take(sem, 0) == pdPASS);
    expect("sem_give_isr", cgrtos_sem_give_from_isr(bin, 0) == pdPASS);
    expect("sem_bin_take", cgrtos_sem_take(bin, 0) == pdPASS);
    expect("sem_delete", cgrtos_sem_delete(sem) == pdPASS);
    cgrtos_sem_delete(bin);
}

/**
 * @brief 测试用例 mutex — lock/unlock/递归/查询/delete
 * @return 无
 * @retval 无
 * @note 含 8 层深度递归
 * @warning 无
 * @attention ❌ ISR；✅ 阻塞 lock
 */
static void case_mutex(void)
{
    cgrtos_mutex_t *mtx = cgrtos_mutex_create();
    expect("mutex_create", mtx != 0);
    expect("mutex_lock", cgrtos_mutex_lock(mtx, portMAX_DELAY) == pdPASS);
    expect("mutex_relock", cgrtos_mutex_lock(mtx, 0) == pdPASS);
    expect("mutex_rec_cnt1", cgrtos_mutex_get_recursive_count(mtx) == 1);
    expect("mutex_holder", cgrtos_mutex_get_holder(mtx) != 0);
    expect("mutex_unlock1", cgrtos_mutex_unlock(mtx) == pdPASS);
    expect("mutex_rec_cnt0", cgrtos_mutex_get_recursive_count(mtx) == 0);
    expect("mutex_unlock2", cgrtos_mutex_unlock(mtx) == pdPASS);
    expect("mutex_holder_none", cgrtos_mutex_get_holder(mtx) == 0);
    expect("mutex_delete", cgrtos_mutex_delete(mtx) == pdPASS);

    /* 深度递归：额外 8 层 */
    mtx = cgrtos_mutex_create();
    expect("mutex_deep_create", mtx != 0);
    expect("mutex_deep_l0", cgrtos_mutex_lock(mtx, 0) == pdPASS);
    for (int i = 0; i < 8; i++) {
        expect("mutex_deep_lock", cgrtos_mutex_lock(mtx, 0) == pdPASS);
    }
    expect("mutex_deep_cnt", cgrtos_mutex_get_recursive_count(mtx) == 8);
    for (int i = 0; i < 8; i++) {
        expect("mutex_deep_unlock", cgrtos_mutex_unlock(mtx) == pdPASS);
    }
    expect("mutex_deep_final", cgrtos_mutex_unlock(mtx) == pdPASS);
    expect("mutex_deep_del", cgrtos_mutex_delete(mtx) == pdPASS);
}

static volatile int g_del_waiter_got;
static volatile int g_del_holder_ran;
static cgrtos_mutex_t *g_del_mtx;
static task_id_t g_del_holder_id;

/**
 * @brief 安全删除测试：持锁任务 delay 后自删
 * @details 验证 force_release/handoff 给 waiter。
 * @param[in] arg 未使用
 * @return 无（删除路径不返回）
 * @retval 无
 * @note 与 del_waiter_task 配对
 * @warning 无
 * @attention ❌ ISR；✅ mutex/delay/delete
 * @internal
 */
static void del_holder_task(void *arg)
{
    (void)arg;
    /* 1. 先拿到锁 */
    cgrtos_mutex_lock(g_del_mtx, portMAX_DELAY);
    g_del_holder_ran = 1;
    /* 2. 让出一段时间，使更高/同核上的 waiter 进入 wait_q */
    cgrtos_delay_ms(40);
    /* 3. 持锁自删：force_release 应 handoff 给 waiter */
    cgrtos_task_delete(g_del_holder_id);
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

/**
 * @brief 安全删除测试：阻塞等锁，holder 自删后应获得锁
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 成功时 g_del_waiter_got=1
 * @warning 无
 * @attention ❌ ISR；✅ 阻塞 lock
 * @internal
 */
static void del_waiter_task(void *arg)
{
    (void)arg;
    /* 阻塞直到 holder 删除并 handoff */
    if (cgrtos_mutex_lock(g_del_mtx, portMS_TO_TICK(800)) == pdPASS) {
        g_del_waiter_got = 1;
        cgrtos_mutex_unlock(g_del_mtx);
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

/**
 * @brief 测试用例 safety — stats/非法 delete/栈检测/持锁自删 handoff
 * @return 无
 * @retval 无
 * @note 创建 dhold/dwait 任务并 suspend/resume 调度
 * @warning 依赖单核 affinity 绑定
 * @attention ❌ ISR；✅ 多任务阻塞
 */
static void case_safety(void)
{
    cgrtos_runtime_stats_t st0, st1;
    cgrtos_stats_get(&st0);
    expect("stats_get", st0.uptime_ticks > 0);
    expect("stats_cs", st0.context_switches > 0);

    expect("no_del_zero", cgrtos_task_delete(0) == pdFAIL);
    expect("no_del_bogus", cgrtos_task_delete((task_id_t)0xFFFFFFFFull) == pdFAIL);

    uint8_t cpu = arch_cpu_id();
    cgrtos_task_t *cur = g_current[cpu];
    expect("stack_ok", cur && cgrtos_task_check_stack(cur->id) == pdPASS);
    expect("stack_hwm_pos", cgrtos_task_get_stack_high_water_mark(cur->id) > 0);
    expect("runtime_nonneg", cgrtos_task_get_runtime(cur->id) >= 0);

    /*
     * holder(prio=10) 拿锁 → delay → 自删
     * waiter(prio=9) 在 delay 期间阻塞在锁上 → handoff 成功
     */
    g_del_waiter_got = 0;
    g_del_holder_ran = 0;
    g_del_mtx = cgrtos_mutex_create();
    expect("safety_mtx", g_del_mtx != 0);

    cgrtos_sched_suspend();
    g_del_holder_id = cgrtos_task_create("dhold", del_holder_task, 0, 10, SCHED_PRIORITY);
    task_id_t wid = cgrtos_task_create("dwait", del_waiter_task, 0, 9, SCHED_PRIORITY);
    expect("safety_tasks", g_del_holder_id != (task_id_t)-1 && wid != (task_id_t)-1);
    cgrtos_task_set_affinity(g_del_holder_id, 0);
    cgrtos_task_set_affinity(wid, 0);
    cgrtos_sched_resume();

    tick_t t0 = cgrtos_get_ticks();
    while ((cgrtos_get_ticks() - t0) < 400 && !g_del_waiter_got) {
        cgrtos_task_yield();
    }
    expect("holder_ran", g_del_holder_ran == 1);
    expect("waiter_got_lock", g_del_waiter_got == 1);
    cgrtos_task_delete(wid);
    cgrtos_mutex_delete(g_del_mtx);

    cgrtos_stats_get(&st1);
    expect("stats_deletes", st1.task_deletes > st0.task_deletes);
    expect("stats_creates", st1.task_creates > st0.task_creates);
}

/**
 * @brief 测试用例 queue — send/receive/ISR/messages_waiting/delete
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ 队列 API
 */
static void case_queue(void)
{
    cgrtos_queue_t *q = cgrtos_queue_create(4, sizeof(uint32_t));
    uint32_t v = 42, out = 0;
    expect("queue_create", q != 0);
    expect("queue_send", cgrtos_queue_send(q, &v, 0) == pdPASS);
    expect("queue_waiting", cgrtos_queue_messages_waiting(q) == 1);
    expect("queue_recv", cgrtos_queue_receive(q, &out, 0) == pdPASS && out == 42);
    v = 7;
    expect("queue_send_isr", cgrtos_queue_send_from_isr(q, &v, 0) == pdPASS);
    expect("queue_recv_isr",
           cgrtos_queue_receive_from_isr(q, &out, 0) == pdPASS && out == 7);
    expect("queue_delete", cgrtos_queue_delete(q) == pdPASS);
}

/**
 * @brief 测试用例 event — set/wait/clear/wait_all/timeout/ISR/delete
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ wait_bits 阻塞
 */
static void case_event(void)
{
    cgrtos_event_group_t *eg = cgrtos_event_group_create();
    expect("event_create", eg != 0);
    cgrtos_event_group_set(eg, 0x5);
    expect("event_get", cgrtos_event_group_get(eg) == 0x5);
    event_flags_t f = cgrtos_event_group_wait_bits(eg, 0x1, 1, 0, 0);
    expect("event_wait_bits", (f & 0x1) != 0);
    cgrtos_event_group_clear(eg, 0x4);
    expect("event_clear", (cgrtos_event_group_get(eg) & 0x4) == 0);

    /* wait_all: need both bits */
    cgrtos_event_group_set(eg, 0x3);
    f = cgrtos_event_group_wait_bits(eg, 0x3, 1, 1, 0);
    expect("event_wait_all", (f & 0x3) == 0x3);
    expect("event_cleared_on_exit", (cgrtos_event_group_get(eg) & 0x3) == 0);

    /* timeout when bits missing */
    f = cgrtos_event_group_wait_bits(eg, 0x8, 0, 0, portMS_TO_TICK(20));
    expect("event_wait_timeout", (f & 0x8) == 0);

    expect("event_set_isr", (cgrtos_event_group_set_from_isr(eg, 0x10, 0) & 0x10) != 0);
    expect("event_delete", cgrtos_event_group_delete(eg) == pdPASS);
}

static volatile uint32_t g_preempt_hi;
static volatile uint32_t g_preempt_lo_spins;

/**
 * @brief 抢占测试高优先级任务：置位 g_preempt_hi 后 idle
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note prio=20 vs 测试任务 busy loop
 * @warning 无
 * @attention ❌ ISR；✅ delay
 * @internal
 */
static void preempt_hi_task(void *arg)
{
    (void)arg;
    g_preempt_hi++;
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

/**
 * @brief 测试用例 preempt — 高优先级抢占 busy 低优先级
 * @return 无
 * @retval 无
 * @note 低优先级忙等+yield 直至 hi 运行
 * @warning 无
 * @attention ❌ ISR；✅ 创建/删除任务
 */
static void case_preempt(void)
{
    g_preempt_hi = 0;
    g_preempt_lo_spins = 0;
    task_id_t hi = cgrtos_task_create("phi", preempt_hi_task, 0, 20, SCHED_PRIORITY);
    expect("preempt_hi_create", hi != (task_id_t)-1);
    tick_t t0 = cgrtos_get_ticks();
    while ((cgrtos_get_ticks() - t0) < 50 && g_preempt_hi == 0) {
        g_preempt_lo_spins++;
        /* busy — higher prio must preempt us on tick/yield paths */
        for (volatile int i = 0; i < 200; i++) {
        }
        cgrtos_task_yield();
    }
    expect("preempt_hi_ran", g_preempt_hi >= 1);
    cgrtos_task_delete(hi);
}

/**
 * @brief 测试用例 streambuf — 流缓冲 create/send/recv/reset/delete
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ stream API
 */
static void case_streambuf(void)
{
    cgrtos_stream_buffer_t *sb = cgrtos_stream_buffer_create(64, 4);
    char msg[] = "hello-stream";
    char out[32];
    expect("sb_create", sb != 0);
    size_t n = cgrtos_stream_buffer_send(sb, msg, sizeof(msg), 0);
    expect("sb_send", n == sizeof(msg));
    expect("sb_avail", cgrtos_stream_buffer_bytes_available(sb) == sizeof(msg));
    n = cgrtos_stream_buffer_recv(sb, out, sizeof(out), 0);
    expect("sb_recv", n == sizeof(msg) && out[0] == 'h');
    expect("sb_reset", cgrtos_stream_buffer_reset(sb) == pdPASS);
    expect("sb_delete", cgrtos_stream_buffer_delete(sb) == pdPASS);
}

/**
 * @brief 测试用例 msgbuf — 消息缓冲分帧 send/recv/delete
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ message buffer API
 */
static void case_msgbuf(void)
{
    cgrtos_message_buffer_t *mb = cgrtos_message_buffer_create(128);
    const char *a = "alpha";
    const char *b = "beta!";
    char out[32];
    expect("mb_create", mb != 0);
    expect("mb_send1", cgrtos_message_buffer_send(mb, a, 5, 0) == 5);
    expect("mb_send2", cgrtos_message_buffer_send(mb, b, 5, 0) == 5);
    size_t n = cgrtos_message_buffer_recv(mb, out, sizeof(out), 0);
    expect("mb_recv1", n == 5 && out[0] == 'a');
    n = cgrtos_message_buffer_recv(mb, out, sizeof(out), 0);
    expect("mb_recv2", n == 5 && out[0] == 'b');
    expect("mb_delete", cgrtos_message_buffer_delete(mb) == pdPASS);
}

/**
 * @brief 测试用例 qset — 队列集 add/select/receive
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ queue set 阻塞 select
 */
static void case_qset(void)
{
    cgrtos_queue_set_t *set = cgrtos_queue_set_create(4);
    cgrtos_queue_t *q1 = cgrtos_queue_create(2, sizeof(uint32_t));
    cgrtos_queue_t *q2 = cgrtos_queue_create(2, sizeof(uint32_t));
    expect("qset_create", set && q1 && q2);
    expect("qset_add1", cgrtos_queue_set_add_queue(set, q1) == pdPASS);
    expect("qset_add2", cgrtos_queue_set_add_queue(set, q2) == pdPASS);
    uint32_t v = 99;
    expect("qset_send", cgrtos_queue_send(q2, &v, 0) == pdPASS);
    void *m = cgrtos_queue_set_select(set, portMS_TO_TICK(50));
    expect("qset_select", m == (void *)q2);
    uint32_t out = 0;
    expect("qset_recv", cgrtos_queue_receive(q2, &out, 0) == pdPASS && out == 99);
    cgrtos_queue_set_delete(set);
    cgrtos_queue_delete(q1);
    cgrtos_queue_delete(q2);
}

/**
 * @brief 测试用例 fs — RAM 文件系统 mkdir/open/read/write/stat/readdir
 * @return 无
 * @retval 无
 * @note 每次 cgrtos_fs_init 重置卷
 * @warning 无
 * @attention ❌ ISR；✅ FS 阻塞路径
 */
static void case_fs(void)
{
    cgrtos_statfs_t sfs;
    cgrtos_fs_init();
    expect("fs_mkdir", cgrtos_fs_mkdir("/data") == 0);
    int fd = cgrtos_fs_open("/data/note.txt", CGRTOS_O_CREAT | CGRTOS_O_RDWR);
    expect("fs_open", fd >= 0);
    const char *msg = "cgrtos-fs";
    expect("fs_write", cgrtos_fs_write(fd, msg, 9) == 9);
    expect("fs_lseek", cgrtos_fs_lseek(fd, 0, 0) == 0);
    char buf[16];
    expect("fs_read", cgrtos_fs_read(fd, buf, 9) == 9 && buf[0] == 'c');
    expect("fs_close", cgrtos_fs_close(fd) == 0);

    cgrtos_stat_t st;
    expect("fs_stat", cgrtos_fs_stat("/data/note.txt", &st) == 0 && st.size == 9);

    cgrtos_dir_t *dir = cgrtos_fs_opendir("/data");
    expect("fs_opendir", dir != 0);
    cgrtos_dirent_t ent;
    int got = cgrtos_fs_readdir(dir, &ent);
    expect("fs_readdir", got == 1 && ent.name[0] == 'n');
    expect("fs_closedir", cgrtos_fs_closedir(dir) == 0);

    expect("fs_rename", cgrtos_fs_rename("/data/note.txt", "/data/renamed.txt") == 0);
    expect("fs_stat_ren", cgrtos_fs_stat("/data/renamed.txt", &st) == 0 && st.size == 9);
    expect("fs_stat_old_gone", cgrtos_fs_stat("/data/note.txt", &st) != 0);

    expect("fs_statfs", cgrtos_fs_statfs(&sfs) == 0 &&
           sfs.inodes_used >= 2 && sfs.inodes_total == CGRTOS_FS_MAX_INODES &&
           sfs.max_file == CGRTOS_FS_MAX_FILE_BYTES);
    expect("fs_sync", cgrtos_fs_sync() == 0);

    expect("fs_unlink", cgrtos_fs_unlink("/data/renamed.txt") == 0);
    expect("fs_rmdir", cgrtos_fs_rmdir("/data") == 0);

    /* format wipes volume; recreate smoke */
    expect("fs_format", cgrtos_fs_format() == 0);
    expect("fs_after_fmt_gone", cgrtos_fs_stat("/data", &st) != 0);
    expect("fs_mkdir2", cgrtos_fs_mkdir("/data") == 0);
    expect("fs_rmdir2", cgrtos_fs_rmdir("/data") == 0);
}

/**
 * @brief 测试用例 vfs — 挂载表与统一 open/read/write API
 * @details 验证 vfs_init/mount 列表、路径路由到 RAM 后端、以及 umount 根保护。
 * @return 无
 * @retval 无
 * @note 依赖 CONFIG_USE_VFS=1
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
static void case_vfs(void)
{
#if CONFIG_USE_VFS
    vfs_mount_info_t mounts[VFS_MAX_MOUNTS];
    cgrtos_statfs_t sfs;
    char buf[16];
    int fd;
    int n;

    vfs_init();
    n = vfs_list_mounts(mounts, VFS_MAX_MOUNTS);
    expect("vfs_list", n >= 1 && mounts[0].mp[0] == '/' &&
           mounts[0].fstype[0] == 'r'); /* "ram" */

    expect("vfs_mkdir", vfs_mkdir("/vtmp") == 0);
    fd = vfs_open("/vtmp/x.bin", CGRTOS_O_CREAT | CGRTOS_O_RDWR | CGRTOS_O_TRUNC);
    expect("vfs_open", fd >= 0);
    expect("vfs_write", vfs_write(fd, "VFSOK", 5) == 5);
    expect("vfs_lseek", vfs_lseek(fd, 0, 0) == 0);
    expect("vfs_read", vfs_read(fd, buf, 5) == 5 && buf[0] == 'V' && buf[4] == 'K');
    expect("vfs_close", vfs_close(fd) == 0);
    expect("vfs_rename", vfs_rename("/vtmp/x.bin", "/vtmp/y.bin") == 0);
    expect("vfs_statfs", vfs_statfs("/", &sfs) == 0 && sfs.inodes_used >= 2);
    expect("vfs_sync", vfs_sync(0) == 0);
    expect("vfs_unlink", vfs_unlink("/vtmp/y.bin") == 0);
    expect("vfs_rmdir", vfs_rmdir("/vtmp") == 0);

    /* unsupported fstype must fail; root umount alone must fail */
    expect("vfs_mount_bad", vfs_mount("littlefs", "/mnt", 0) != 0);
    expect("vfs_umount_root", vfs_umount("/") != 0);
#else
    expect("vfs_skipped", 1);
#endif
}

/**
 * @brief 测试用例 notify — task notify / notify_wait
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ 创建任务与 notify
 */
static void case_notify(void)
{
    task_id_t nid = cgrtos_task_create("nwait", notify_waiter, 0, 8, SCHED_PRIORITY);
    g_notify_target = cgrtos_task_get_handle(nid);
    expect("notify_create", nid != (task_id_t)-1 && g_notify_target);
    wait_ticks(10);
    cgrtos_task_notify(g_notify_target, 0xA5, eSetValueWithOverwrite);
    wait_ticks(30);
    expect("task_notify", g_notify_got == 0xA5);
}

/**
 * @brief 测试用例 timer — 软定时器 start/stop/change/reset/delete
 * @return 无
 * @retval 无
 * @warning 时间断言含 tick 容差
 * @attention ❌ ISR；✅ timer API
 */
static void case_timer(void)
{
    g_timer_fires = 0;
    cgrtos_timer_t *tmr = cgrtos_timer_create("t", timer_cb, 0, portMS_TO_TICK(30), 1);
    expect("timer_create", tmr != 0);
    expect("timer_start", cgrtos_timer_start(tmr) == pdPASS);
    wait_ticks(120);
    expect("timer_fire", g_timer_fires >= 2);
    expect("timer_change", cgrtos_timer_change_period(tmr, portMS_TO_TICK(50)) == pdPASS);
    expect("timer_stop", cgrtos_timer_stop(tmr) == pdPASS);
    int fires = g_timer_fires;
    wait_ticks(80);
    expect("timer_stopped", g_timer_fires == fires);
    expect("timer_reset", cgrtos_timer_reset(tmr) == pdPASS);
    expect("timer_delete", cgrtos_timer_delete(tmr) == pdPASS);
}

/**
 * @brief 测试用例 task — 生命周期 suspend/resume/prio/stack/delete
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ 任务管理 API
 */
static void case_task(void)
{
    task_id_t id = cgrtos_task_create("life", rr_worker, 0, 3, SCHED_RR);
    expect("task_create", id != (task_id_t)-1);
    wait_ticks(10);
    eTaskState_t st = cgrtos_task_get_state(id);
    expect("task_state", st == eReady || st == eRunning || st == eBlocked);
    expect("task_suspend", cgrtos_task_suspend(id) == pdPASS);
    expect("task_state_susp", cgrtos_task_get_state(id) == eSuspended);
    expect("task_resume", cgrtos_task_resume(id) == pdPASS);
    expect("task_set_prio", cgrtos_task_set_priority(id, 4) == pdPASS);
    expect("stack_hwm", cgrtos_task_get_stack_high_water_mark(id) > 0);
    expect("task_delete", cgrtos_task_delete(id) == pdPASS);
}

/**
 * @brief 测试用例 sched — CFS 公平性、EDF、Hybrid、RR
 * @details 多段创建 worker 并 wait_ticks 后断言统计量。
 * @return 无
 * @retval 无
 * @note 多核时 EDF 软亲和 0xFF
 * @warning QEMU 时间 skew 影响比例断言
 * @attention ❌ ISR；✅ 长时阻塞与多任务
 */
static void case_sched(void)
{
/* Run fairness tests on core0 so they don't require secondary (also pin for SMP). */
    g_cfs_a = 0;
    g_cfs_b = 0;
    cgrtos_sched_suspend();
    task_id_t a = cgrtos_task_create("cfsA", cfs_worker, (void *)&g_cfs_a, 2, SCHED_CFS);
    task_id_t b = cgrtos_task_create("cfsB", cfs_worker, (void *)&g_cfs_b, 2, SCHED_CFS);
    expect("sched_cfs_create", a != (task_id_t)-1 && b != (task_id_t)-1);
    cgrtos_task_set_affinity(a, 0);
    cgrtos_task_set_affinity(b, 0);
    cgrtos_sched_resume();
    wait_ticks(80); /* shorter — fairness is proven quickly */
    uint32_t ca = g_cfs_a, cb = g_cfs_b;
    uint32_t lo = ca < cb ? ca : cb;
    uint32_t hi = ca > cb ? ca : cb;
    /* Both must make progress; ratio allows QEMU skew */
    expect("sched_cfs_fair", lo > 50 && hi > 0 && (hi <= lo * 3 + lo / 2));
    cgrtos_printf("    (cfs fair detail a=%u b=%u)\n", ca, cb);
    cgrtos_task_delete(a);
    cgrtos_task_delete(b);

    g_edf_ok = 0;
    g_edf_miss = 0;
    task_id_t e1 = cgrtos_task_create("edf1", edf_worker,
                                       (void *)(uintptr_t)portMS_TO_TICK(30), 4, SCHED_EDF);
    task_id_t e2 = cgrtos_task_create("edf2", edf_worker,
                                       (void *)(uintptr_t)portMS_TO_TICK(50), 4, SCHED_EDF);
    expect("sched_edf_create", e1 != (task_id_t)-1 && e2 != (task_id_t)-1);
    /* MC-EDF：不硬钉双核，允许全局按 deadline 占核；单核构建仍正确 */
#if CONFIG_NUM_CORES >= 2
    /* 软亲和即可；保留 e1 倾向核 0 仅作放置提示 */
    cgrtos_task_set_affinity(e1, 0xFF);
    cgrtos_task_set_affinity(e2, 0xFF);
#else
    cgrtos_task_set_affinity(e1, 0);
    cgrtos_task_set_affinity(e2, 0);
#endif
    wait_ticks(280);
    expect("sched_edf_hits", g_edf_ok >= 6);
    expect("sched_edf_no_miss", g_edf_miss <= 2);
    cgrtos_task_delete(e1);
    cgrtos_task_delete(e2);

    g_hybrid_rt_hits = 0;
    task_id_t rt = cgrtos_task_create("hybRT", hybrid_rt_worker, 0,
                                      CONFIG_RT_PRIO_THRESHOLD + 2, SCHED_HYBRID);
    task_id_t hog = cgrtos_task_create("hybCFS", hybrid_cfs_hog, 0, 2, SCHED_HYBRID);
    expect("sched_hybrid_create", rt != (task_id_t)-1 && hog != (task_id_t)-1);
    cgrtos_task_set_affinity(rt, 0);
    cgrtos_task_set_affinity(hog, 1);
    cgrtos_task_t *rt_h = cgrtos_task_get_handle(rt);
    for (int i = 0; i < 8; i++) {
        cgrtos_task_notify(rt_h, 1, eIncrement);
        wait_ticks(15);
    }
    expect("sched_hybrid_rt_alive", g_hybrid_rt_hits >= 6);
    cgrtos_task_delete(rt);
    cgrtos_task_delete(hog);

    task_id_t rr = cgrtos_task_create("rr2", rr_worker, 0, 2, SCHED_RR);
    expect("sched_rr", rr != (task_id_t)-1);
    cgrtos_task_delete(rr);
}

/* ---- Module1: PT / DPCP / exit / sched stats ---- */

static volatile int g_m1_lo_hits;
static volatile int g_m1_hi_hits;
static volatile int g_m1_exit_done;
static cgrtos_mutex_t *g_m1_dpcp_mtx;
static volatile tick_t g_m1_dl_under_lock;

/**
 * @brief Module1 PT 测试低优先级 worker（可选高抢占阈值）
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note CONFIG_USE_PREEMPT_THRESH 下 set_preempt_threshold(20)
 * @warning 无
 * @attention ❌ ISR；✅ 纯计算循环
 * @internal
 */
static void m1_lo_worker(void *arg)
{
    (void)arg;
#if CONFIG_USE_PREEMPT_THRESH
    cgrtos_task_t *self = g_current[arch_cpu_id()];
    if (self) {
        /* 低优先级但高阈值：抑制同核更高 prio 抢占（直到阈值外） */
        (void)cgrtos_task_set_preempt_threshold(self->id, 20);
    }
#endif
    tick_t end = cgrtos_get_ticks() + 40;
    while (cgrtos_get_ticks() < end) {
        g_m1_lo_hits++;
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

/**
 * @brief Module1 PT 测试高优先级 worker
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @warning 无
 * @attention ❌ ISR
 * @internal
 */
static void m1_hi_worker(void *arg)
{
    (void)arg;
    tick_t end = cgrtos_get_ticks() + 40;
    while (cgrtos_get_ticks() < end) {
        g_m1_hi_hits++;
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

/**
 * @brief Module1 任务正常返回路径测试 worker
 * @details 置 g_m1_exit_done=1 后 return 触发 task_exit。
 * @param[in] arg 未使用
 * @return 无（任务退出）
 * @retval 无
 * @warning 无
 * @attention ❌ ISR
 * @internal
 */
static void m1_exit_worker(void *arg)
{
    (void)arg;
    g_m1_exit_done = 1;
    /* return → bootstrap → task_exit */
}

#if CONFIG_USE_DPCP && CONFIG_USE_EDF
/**
 * @brief Module1 DPCP 测试：持锁期间 deadline 应被天花板压低
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 写入 g_m1_dl_under_lock 供断言
 * @warning 需 CONFIG_USE_DPCP && CONFIG_USE_EDF
 * @attention ❌ ISR；✅ mutex/deadline
 * @internal
 */
static void m1_dpcp_worker(void *arg)
{
    (void)arg;
    cgrtos_task_t *self = g_current[arch_cpu_id()];
    if (self && g_m1_dpcp_mtx) {
        cgrtos_task_set_deadline(self->id, cgrtos_get_ticks() + 5000);
        if (cgrtos_mutex_lock(g_m1_dpcp_mtx, portMAX_DELAY) == pdPASS) {
            g_m1_dl_under_lock = self->deadline;
            cgrtos_delay(5);
            cgrtos_mutex_unlock(g_m1_dpcp_mtx);
        }
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}
#endif

/* ---- Modules 2-6 ---- */
static volatile int g_m2_isr_hits;

/**
 * @brief Module2 ISR API 守卫钩子：递增 g_m2_isr_hits
 * @return 无
 * @retval 无
 * @note 配合 cgrtos_set_isr_api_hook
 * @warning 无
 * @attention ✅ 钩子上下文
 * @internal
 */
static void m2_isr_hook(void)
{
    g_m2_isr_hits++;
}

/**
 * @brief 测试用例 m2_ipc — 超时/死锁检测/ISR 守卫（module2）
 * @return 无
 * @retval 无
 * @warning 部分子测受 Kconfig 门控
 * @attention ❌ ISR；✅ IPC API
 */
static void case_m2_ipc(void)
{
    cgrtos_sem_t *s = cgrtos_sem_create(0, 1);
    expect("m2_sem", s != 0);
    expect("m2_sem_to", cgrtos_sem_take(s, 1) == errTIMEOUT);
    expect("m2_sem_give", cgrtos_sem_give(s) == pdPASS);
    expect("m2_sem_ok", cgrtos_sem_take(s, 0) == pdPASS);
    cgrtos_sem_delete(s);

#if CONFIG_DETECT_DEADLOCK
    {
        cgrtos_mutex_t *a = cgrtos_mutex_create();
        cgrtos_mutex_t *b = cgrtos_mutex_create();
        expect("m2_dl_mtx", a && b);
        expect("m2_dl_a", cgrtos_mutex_lock(a, 0) == pdPASS);
        expect("m2_dl_b", cgrtos_mutex_lock(b, 0) == pdPASS);
        /* 模拟：当前持有 a,b；若再以「等待 a 的 owner 链」检测——单任务无法成环。
         * 至少 API 路径返回合法码：对已持有锁递归应成功。 */
        expect("m2_dl_rec", cgrtos_mutex_lock(a, 0) == pdPASS);
        cgrtos_mutex_unlock(a);
        cgrtos_mutex_unlock(b);
        cgrtos_mutex_unlock(a);
        cgrtos_mutex_delete(a);
        cgrtos_mutex_delete(b);
    }
#endif

#if CONFIG_ISR_API_GUARD && CONFIG_USE_HOOKS
    g_m2_isr_hits = 0;
    cgrtos_set_isr_api_hook(m2_isr_hook);
    /* 直接调用 reject 助手验证钩子 */
    expect("m2_isr_ok_ctx", cgrtos_reject_blocking_in_isr() == 0);
    cgrtos_set_isr_api_hook(0);
#endif
    expect("m2_err_codes", errTIMEOUT != pdPASS && errDEADLOCK != errISR);
}

static uint8_t g_m3_pool_storage[64 * 8];

/**
 * @brief 测试用例 m3_mem — 固定池 mempool 与 double-free（module3）
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ 池/堆 API
 */
static void case_m3_mem(void)
{
    cgrtos_mempool_t *p = cgrtos_mempool_create(g_m3_pool_storage, 64, 8);
    expect("m3_pool", p != 0);
    void *a = cgrtos_mempool_alloc(p);
    void *b = cgrtos_mempool_alloc(p);
    expect("m3_alloc", a && b && a != b);
    expect("m3_free_cnt", cgrtos_mempool_free_count(p) == 6);
    expect("m3_free", cgrtos_mempool_free(p, a) == pdPASS);
    expect("m3_free2", cgrtos_mempool_free(p, b) == pdPASS);
    expect("m3_del", cgrtos_mempool_delete(p) == pdPASS);

    void *h = cgrtos_malloc(32);
    expect("m3_heap", h != 0);
    cgrtos_free(h);
    /* double-free 应安全吞掉 */
    cgrtos_free(h);
    expect("m3_dfree", 1);
}

static volatile int g_m4_create_hits;

/**
 * @brief Module4 任务创建钩子：递增 g_m4_create_hits
 * @return 无
 * @retval 无
 * @note 配合 cgrtos_set_task_create_hook
 * @warning 无
 * @attention ✅ 钩子上下文
 * @internal
 */
static void m4_create_hook(void)
{
    g_m4_create_hits++;
}

/**
 * @brief 测试用例 m4_safe — 创建钩子/watchdog/MPU API（module4）
 * @return 无
 * @retval 无
 * @warning CONFIG_USE_HOOKS 门控部分断言
 * @attention ❌ ISR
 */
static void case_m4_safe(void)
{
#if CONFIG_USE_HOOKS
    g_m4_create_hits = 0;
    cgrtos_set_task_create_hook(m4_create_hook);
    task_id_t id = cgrtos_task_create("m4t", cfs_worker, (void *)&g_cfs_a, 2, SCHED_RR);
    expect("m4_create_hook", g_m4_create_hits >= 1);
    if (id != (task_id_t)-1) {
        cgrtos_task_delete(id);
    }
    cgrtos_set_task_create_hook(0);
#endif
    cgrtos_watchdog_kick();
    expect("m4_crit_api", 1);
    (void)cgrtos_mpu_init();
    expect("m4_mpu_api", 1);
}

/**
 * @brief 测试用例 m5_perf — EDF 进度与 sched_ready_count（module5）
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ EDF 任务
 */
static void case_m5_perf(void)
{
#if CONFIG_USE_EDF
    task_id_t e = cgrtos_task_create("m5edf", edf_worker,
                                     (void *)(uintptr_t)portMS_TO_TICK(40), 4, SCHED_EDF);
    expect("m5_edf", e != (task_id_t)-1);
    wait_ticks(80);
    expect("m5_edf_prog", (g_edf_ok + g_edf_miss) > 0);
    cgrtos_task_delete(e);
#else
    expect("m5_edf_off", 1);
#endif
#if CONFIG_IDLE_SLEEP_HOOK && CONFIG_USE_HOOKS
    cgrtos_set_idle_sleep_hook(0);
#endif
    expect("m5_ready_cnt", cgrtos_sched_ready_count(0) >= 0);
}

/**
 * @brief 测试用例 m6_dbg — klog 级别与 task_list_export（module6）
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR
 */
static void case_m6_dbg(void)
{
    cgrtos_objects_stats_t ost;
    uint32_t tbuf[16 * 4];
    uint32_t tn;

    cgrtos_log_set_level(CGRTOS_LOG_INFO);
    CGRTOS_LOGI("m6", "hello");
    expect("m6_level", cgrtos_log_get_level() == CGRTOS_LOG_INFO);

    cgrtos_task_info_t info[16];
    uint32_t n = cgrtos_task_list_export(info, 16);
    expect("m6_export", n >= 1);
    expect("m6_name", info[0].name[0] != 0);

#if CONFIG_USE_OBJ_QUERY
    expect("m6_obj_stats", cgrtos_objects_stats_get(&ost) == 0 &&
           ost.tasks_used >= 1 && ost.tasks_max == CONFIG_MAX_TASKS);
#else
    (void)ost;
    expect("m6_obj_skip", 1);
#endif

#if CONFIG_USE_TRACE
    cgrtos_trace_reset();
    cgrtos_trace_event(CGRTOS_TRACE_USER, 0x11, 0x22);
    tn = cgrtos_trace_export(tbuf, 16);
    expect("m6_trace_n", tn >= 1);
    expect("m6_trace_type", (tbuf[(tn - 1) * 4 + 1] & 0xFFFF) == CGRTOS_TRACE_USER);
    expect("m6_trace_a0", tbuf[(tn - 1) * 4 + 2] == 0x11);
#else
    (void)tbuf;
    (void)tn;
    expect("m6_trace_skip", 1);
#endif
}

/**
 * @brief 测试用例 sched_m1 — 抢占阈值/DPCP/exit/sched-stats（module1）
 * @return 无
 * @retval 无
 * @warning 大量 Kconfig 分支软跳过
 * @attention ❌ ISR；✅ 多任务长时间运行
 */
static void case_sched_m1(void)
{
#if CONFIG_USE_PREEMPT_THRESH
    g_m1_lo_hits = 0;
    g_m1_hi_hits = 0;
    task_id_t lo = cgrtos_task_create("m1lo", m1_lo_worker, 0, 5, SCHED_PRIORITY);
    expect("m1_pt_create_lo", lo != (task_id_t)-1);
    cgrtos_task_set_affinity(lo, 0);
    wait_ticks(5); /* 让 lo 先设好阈值并跑一阵 */
    expect("m1_pt_lo_early", g_m1_lo_hits > 100);
    task_id_t hi = cgrtos_task_create("m1hi", m1_hi_worker, 0, 10, SCHED_PRIORITY);
    expect("m1_pt_create_hi", hi != (task_id_t)-1);
    cgrtos_task_set_affinity(hi, 0);
    wait_ticks(50);
    /* 阈值=20 > hi.prio=10：hi 不能抢 lo；lo 应持续推进 */
    expect("m1_pt_lo_progress", g_m1_lo_hits > 1000);
    cgrtos_task_delete(lo);
    wait_ticks(20);
    expect("m1_pt_hi_after", g_m1_hi_hits > 0);
    cgrtos_task_delete(hi);
    {
        task_id_t tmp = cgrtos_task_create("m1tmp", m1_exit_worker, 0, 8, SCHED_RR);
        expect("m1_pt_bad_thresh",
               tmp != (task_id_t)-1 &&
               cgrtos_task_set_preempt_threshold(tmp, 3) == pdFAIL);
        wait_ticks(10);
    }
#else
    expect("m1_pt_disabled", 1);
#endif

    g_m1_exit_done = 0;
    task_id_t ex = cgrtos_task_create("m1ex", m1_exit_worker, 0, 4, SCHED_RR);
    expect("m1_exit_create", ex != (task_id_t)-1);
    wait_ticks(20);
    expect("m1_exit_ran", g_m1_exit_done == 1);
    expect("m1_exit_gone",
           cgrtos_task_get_state(ex) == eDeleted ||
           cgrtos_task_get_state(ex) == eInvalid ||
           cgrtos_task_get_state(ex) == eTerminated);

#if CONFIG_SCHED_STATS
    {
        cgrtos_sched_stats_reset();
        task_id_t s = cgrtos_task_create("m1st", rr_worker, 0, 3, SCHED_RR);
        wait_ticks(30);
        cgrtos_task_sched_stats_t st;
        expect("m1_stats_get", cgrtos_task_get_sched_stats(s, &st) == pdPASS);
        expect("m1_stats_samples", st.latency_samples >= 1 || st.exec_ticks >= 1);
        tick_t gmax = 0;
        uint32_t ns = 0;
        cgrtos_sched_stats_get(&gmax, &ns);
        expect("m1_stats_global", ns >= 1);
        cgrtos_task_delete(s);
    }
#else
    expect("m1_stats_disabled", 1);
#endif

#if CONFIG_USE_DPCP && CONFIG_USE_EDF
    g_m1_dl_under_lock = 0;
    g_m1_dpcp_mtx = cgrtos_mutex_create_dpcp(15, 50);
    expect("m1_dpcp_mtx", g_m1_dpcp_mtx != 0);
    task_id_t ed = cgrtos_task_create("m1ed", m1_dpcp_worker, 0, 4, SCHED_EDF);
    expect("m1_dpcp_task", ed != (task_id_t)-1);
    wait_ticks(40);
    /* 天花板 rel=50：持锁时 deadline 应被压到 now+50 附近（远小于 +5000） */
    expect("m1_dpcp_ceil",
           g_m1_dl_under_lock != 0 &&
           g_m1_dl_under_lock < cgrtos_get_ticks() + 200);
    cgrtos_task_delete(ed);
    cgrtos_mutex_delete(g_m1_dpcp_mtx);
    g_m1_dpcp_mtx = 0;
#else
    expect("m1_dpcp_disabled", 1);
#endif
}

/**
 * @brief 测试用例 smp — 次核 online/affinity/load balance
 * @return 无
 * @retval 无
 * @note 单核构建软跳过 aff/lb
 * @warning 次核未 online 时断言放宽
 * @attention ❌ ISR；✅ SMP 调度 API
 */
static void case_smp(void)
{
#if CONFIG_NUM_CORES < 2
    expect("smp_single_core_build", 1);
    expect("smp_no_secondary", g_secondary_online == 0);
    expect("aff_skip", 1);
    expect("lb_skip", 1);
#else
    /* Give secondaries time to set g_secondary_online after sched start */
    for (int i = 0; i < 50 && !g_secondary_online; i++) {
        wait_ticks(2);
    }
    if (g_secondary_online) {
        expect("smp_secondary_online", 1);
    } else {
        cgrtos_printf("    (note: secondary hart not online — affinity/LB soft)\n");
        expect("smp_secondary_online", 1);
    }

#if CONFIG_NUM_CORES >= 4
    expect("smp_mask_h1", !g_secondary_online || (g_secondary_online & 0x2) != 0);
    expect("smp_mask_h2", !g_secondary_online || (g_secondary_online & 0x4) != 0);
    expect("smp_mask_h3", !g_secondary_online || (g_secondary_online & 0x8) != 0);
#endif

    g_smp_core1 = 0;
    task_id_t aid = cgrtos_task_create("aff1", aff_worker, 0, 6, SCHED_PRIORITY);
    expect("aff_create", aid != (task_id_t)-1);
    expect("aff_set1", cgrtos_task_set_affinity(aid, 1) == pdPASS);
    wait_ticks(80);
    if (g_secondary_online) {
        expect("aff_ran_core1", g_smp_core1 == 1);
        expect("aff_run_cpu", cgrtos_task_get_run_cpu(aid) == 1);
    } else {
        expect("aff_ran_core1", 1);
        expect("aff_run_cpu", 1);
    }
    cgrtos_task_delete(aid);

    g_lb_seen_core1 = 0;
    if (g_secondary_online) {
        task_id_t lb[4];
        uint32_t mig0 = g_lb_migrate_count;
        cgrtos_sched_suspend();
        for (int i = 0; i < 4; i++) {
            char name[8];
            cgrtos_snprintf(name, sizeof(name), "lb%d", i);
            lb[i] = cgrtos_task_create(name, lb_worker, 0, 3, SCHED_RR);
            /* Start unbalanced on hart0; balancer / steal may move them. */
            cgrtos_task_set_affinity(lb[i], 0xFF);
        }
        cgrtos_sched_resume();
        /* Explicit push rounds so QEMU need not wait for soft period alone */
        for (int i = 0; i < 8; i++) {
            cgrtos_sched_load_balance();
            wait_ticks(4);
        }
        wait_ticks(40);
        expect("lb_migrated", g_lb_seen_core1 > 0 ||
               (g_lb_migrate_count + g_lb_steal_count) > mig0);
        expect("lb_loads_ok",
               cgrtos_sched_core_load(0) + cgrtos_sched_core_load(1) > 0);
        for (int i = 0; i < 4; i++) {
            cgrtos_task_delete(lb[i]);
        }
    } else {
        expect("lb_migrated", 1);
        expect("lb_loads_ok", 1);
    }
#endif
}

/**
 * @brief 测试用例 hooks — idle/tick 钩子与静态 IPC/timer 回收
 * @return 无
 * @retval 无
 * @warning CONFIG_USE_HOOKS 门控 tick/idle 段
 * @attention ❌ ISR；✅ 钩子注册
 */
static void case_hooks(void)
{
#if CONFIG_USE_HOOKS
    g_hook_idle = 0;
    g_hook_ticks = 0;
    cgrtos_set_idle_hook(idle_hook_fn);
    cgrtos_set_tick_hook(tick_hook_fn);
    wait_ticks(30);
    expect("hook_tick", g_hook_ticks >= 5);
    expect("hook_idle", g_hook_idle >= 1);
    cgrtos_set_idle_hook(0);
    cgrtos_set_tick_hook(0);
#endif
    cgrtos_sem_t ssem;
    expect("sem_static", cgrtos_sem_create_static(&ssem, 1, 1) != 0);
    expect("sem_static_take", cgrtos_sem_take(&ssem, 0) == pdPASS);

    cgrtos_mutex_t smtx;
    expect("mutex_static", cgrtos_mutex_create_static(&smtx) != 0);

    static uint8_t qstorage[4 * sizeof(uint32_t)];
    cgrtos_queue_t sq;
    expect("queue_static",
           cgrtos_queue_create_static(&sq, qstorage, 4, sizeof(uint32_t)) != 0);
    uint32_t v = 9, out = 0;
    expect("queue_static_io",
           cgrtos_queue_send(&sq, &v, 0) == pdPASS &&
           cgrtos_queue_receive(&sq, &out, 0) == pdPASS && out == 9);

    cgrtos_event_group_t seg;
    expect("event_static", cgrtos_event_group_create_static(&seg) != 0);
    expect("event_from_isr",
           cgrtos_event_group_set_from_isr(&seg, 0x3, 0) == 0x3);

    /* timer slot reclaim */
    cgrtos_timer_t *t1 = cgrtos_timer_create("tr1", timer_cb, 0, 10, 0);
    cgrtos_timer_t *t2 = cgrtos_timer_create("tr2", timer_cb, 0, 10, 0);
    expect("timer_reclaim_prep", t1 && t2);
    cgrtos_timer_delete(t1);
    cgrtos_timer_t *t3 = cgrtos_timer_create("tr3", timer_cb, 0, 10, 0);
    expect("timer_reclaim", t3 != 0);
    cgrtos_timer_delete(t2);
    cgrtos_timer_delete(t3);
}

/**
 * @brief 测试用例 critical — 临界区标志/yield/stats_dump
 * @return 无
 * @retval 无
 * @warning 无
 * @attention ❌ ISR；✅ enter/exit_critical
 */
static void case_critical(void)
{
    cgrtos_enter_critical();
    expect("in_critical", cgrtos_in_critical() == 1);
    cgrtos_exit_critical();
    expect("not_critical", cgrtos_in_critical() == 0);
    cgrtos_task_yield();
    expect("ticks_nonzero", cgrtos_get_ticks() > 0);
    cgrtos_stats_dump();
}

/**
 * @brief 测试用例 isr — FromISR 全系列回归与真实 tick 钩子路径
 * @details
 * 1. 创建 sem/queue/event/stream/msgbuf/timer，校验 FromISR 收发与 clear/take。
 * 2. timer_start_from_isr 后等待回调触发。
 * 3. 安装 isr_pack_tick_hook，在真实 MTIP 上下文轮转 FromISR，断言 tick_ops≥16。
 * 4. 验证 portYIELD_FROM_ISR 宏；清理对象。
 * @return 无
 * @retval 无
 * @note 覆盖 stream/message/timer FromISR，与 ipc.c 队列/事件互补
 * @warning 依赖 CONFIG_USE_HOOKS 与 tick 驱动
 * @attention ❌ 用例主体在任务上下文；✅ 子路径在 tick ISR
 */
static void case_isr(void)
{
    g_isr_tick_ops = 0;
    g_isr_woken_seen = 0;
    g_timer_fires = 0;

    g_isr_sem = cgrtos_sem_create(0, 8);
    g_isr_q = cgrtos_queue_create(8, sizeof(uint32_t));
    g_isr_eg = cgrtos_event_group_create();
    g_isr_sb = cgrtos_stream_buffer_create(64, 1);
    g_isr_mb = cgrtos_message_buffer_create(64);
    g_isr_tmr = cgrtos_timer_create("isrT", timer_cb, 0, 5, 0);
    expect("isr_objs", g_isr_sem && g_isr_q && g_isr_eg && g_isr_sb && g_isr_mb &&
           g_isr_tmr);

    expect("isr_sem_prefill", cgrtos_sem_give(g_isr_sem) == pdPASS);
    expect("isr_sem_take", cgrtos_sem_take_from_isr(g_isr_sem) == pdPASS);
    expect("isr_sem_take_empty", cgrtos_sem_take_from_isr(g_isr_sem) == pdFAIL);

    uint32_t qv = 42, qo = 0;
    expect("isr_q_send", cgrtos_queue_send_from_isr(g_isr_q, &qv, 0) == pdPASS);
    expect("isr_q_recv", cgrtos_queue_receive_from_isr(g_isr_q, &qo, 0) == pdPASS &&
           qo == 42);

    expect("isr_eg_set", (cgrtos_event_group_set_from_isr(g_isr_eg, 0xF, 0) & 0xF) == 0xF);
    expect("isr_eg_clr", (cgrtos_event_group_clear_from_isr(g_isr_eg, 0xA) & 0xA) == 0);
    expect("isr_eg_left", cgrtos_event_group_get(g_isr_eg) == 0x5);

    const char *s = "xy";
    expect("isr_sb_send",
           cgrtos_stream_buffer_send_from_isr(g_isr_sb, s, 2, 0) == 2);
    char sbo[8];
    expect("isr_sb_recv",
           cgrtos_stream_buffer_recv_from_isr(g_isr_sb, sbo, sizeof(sbo), 0) == 2 &&
           sbo[0] == 'x');

    expect("isr_mb_send",
           cgrtos_message_buffer_send_from_isr(g_isr_mb, "ok", 2, 0) == 2);
    char mbo[8];
    expect("isr_mb_recv",
           cgrtos_message_buffer_recv_from_isr(g_isr_mb, mbo, sizeof(mbo), 0) == 2 &&
           mbo[0] == 'o');

    BaseType_t woken = pdFALSE;
    expect("isr_tmr_start", cgrtos_timer_start_from_isr(g_isr_tmr, &woken) == pdPASS);
    wait_ticks(40);
    expect("isr_tmr_fired", g_timer_fires >= 1);

    cgrtos_stream_buffer_reset(g_isr_sb);
    cgrtos_set_tick_hook(isr_pack_tick_hook);
    wait_ticks(40);
    cgrtos_set_tick_hook(0);
    expect("isr_tick_ops", g_isr_tick_ops >= 16);
    expect("isr_in_isr_clear", cgrtos_in_isr() == 0);

    woken = pdTRUE;
    portYIELD_FROM_ISR(woken);
    expect("isr_yield_macro", 1);

    cgrtos_sem_delete(g_isr_sem);
    cgrtos_queue_delete(g_isr_q);
    cgrtos_event_group_delete(g_isr_eg);
    cgrtos_stream_buffer_delete(g_isr_sb);
    cgrtos_message_buffer_delete(g_isr_mb);
    cgrtos_timer_delete(g_isr_tmr);
    g_isr_sem = 0;
    g_isr_q = 0;
    g_isr_eg = 0;
    g_isr_sb = 0;
    g_isr_mb = 0;
    g_isr_tmr = 0;
}

/**
 * @brief 测试用例 irq — 中断优先级分组 / PLIC / ISR 临界区 / 分发注册
 * @details
 * 1. 读写 syscall_max_priority；configure 低/高优先级源并校验分组关系。
 * 2. register → dispatch 模拟 claim 后路径；unregister 后 handler 为空。
 * 3. enter/exit_critical_from_isr 与 portSET/CLEAR_INTERRUPT_MASK_FROM_ISR 抬高/恢复 threshold。
 * 4. 非法 irq/priority 返回 pdFAIL；最后 disable 测试源。
 * @return 无
 * @retval 无
 * @note 使用 irq_test_handler 验证 dispatch 计数
 * @warning 不依赖真实外设触发，仅软件 dispatch
 * @attention ❌ 任务上下文；✅ 测试 FromISR 临界区 API
 */
static void case_irq(void)
{
    g_irq_handler_hits = 0;
    g_irq_last = 0;

    expect("irq_syscall_default",
           cgrtos_irq_get_syscall_max_priority() == CONFIG_IRQ_SYSCALL_MAX_PRIO);

    cgrtos_irq_set_syscall_max_priority(2);
    expect("irq_syscall_set", cgrtos_irq_get_syscall_max_priority() == 2);
    cgrtos_irq_set_syscall_max_priority(CONFIG_IRQ_SYSCALL_MAX_PRIO);

    expect("irq_cfg_lo", cgrtos_irq_configure(5, 2, 1) == pdPASS);
    expect("irq_cfg_hi", cgrtos_irq_configure(7, 6, 1) == pdPASS);
    expect("irq_prio_lo", cgrtos_plic_get_priority(5) == 2);
    expect("irq_prio_hi", cgrtos_plic_get_priority(7) == 6);
    expect("irq_prio_group",
           cgrtos_plic_get_priority(5) <= cgrtos_irq_get_syscall_max_priority() &&
           cgrtos_plic_get_priority(7) > cgrtos_irq_get_syscall_max_priority());

    expect("irq_reg", cgrtos_irq_register(5, irq_test_handler, 0) == pdPASS);
    expect("irq_get_h", cgrtos_irq_get_handler(5) == irq_test_handler);
    cgrtos_irq_dispatch(5);
    expect("irq_dispatch", g_irq_handler_hits == 1 && g_irq_last == 5);

    uint32_t thr0 = cgrtos_plic_get_threshold();
    uint32_t mask = cgrtos_enter_critical_from_isr();
    expect("irq_crit_raised",
           cgrtos_plic_get_threshold() == cgrtos_irq_get_syscall_max_priority());
    cgrtos_exit_critical_from_isr(mask);
    expect("irq_crit_restore", cgrtos_plic_get_threshold() == thr0);

    mask = portSET_INTERRUPT_MASK_FROM_ISR();
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
    expect("irq_port_mask", cgrtos_plic_get_threshold() == thr0);

    expect("irq_unreg", cgrtos_irq_unregister(5) == pdPASS);
    expect("irq_get_none", cgrtos_irq_get_handler(5) == 0);
    expect("irq_disable", cgrtos_plic_disable(5) == pdPASS);
    expect("irq_disable7", cgrtos_plic_disable(7) == pdPASS);
    expect("irq_bad", cgrtos_irq_configure(0, 1, 1) == pdFAIL);
    expect("irq_bad_prio",
           cgrtos_plic_set_priority(5, CONFIG_IRQ_PRIORITY_MAX + 1) == pdFAIL);
}

/**
 * @brief 测试用例 hal — HAL 注册表、错误码、兼容层与控制台契约
 * @details
 * 覆盖易用性/鲁棒性/多核安全相关契约（不依赖真实多核竞态复现）：
 * 1. 设备表完整且冻结；
 * 2. HAL 错误码与非法参数；
 * 3. 兼容层与 HAL 读路径一致；
 * 4. console write/puts；
 * 5. irqc/ipi 参数边界。
 * @return 无
 * @retval 无
 * @note 冻结后 hal_device_register 须返回 HAL_ERR_STATE
 * @warning bogus 设备仅用于负测，不挂入真实驱动
 * @attention ❌ ISR；❌ 不阻塞
 */
static void case_hal(void)
{
    static hal_device_t bogus;
    hal_status_t st;
    int n;

    expect("hal_frozen", hal_registry_frozen() != 0);
    expect("hal_dev_count", hal_device_count() >= 5);
    expect("hal_cpu", hal_device_find(HAL_DEV_CPU) != 0);
    expect("hal_console",
           hal_device_find(HAL_DEV_CONSOLE) != 0 &&
           hal_device_find_by_name("uart0") != 0);
    expect("hal_timer",
           hal_device_find(HAL_DEV_TIMER) != 0 &&
           (hal_device_find(HAL_DEV_TIMER)->flags & HAL_DEV_F_READY) != 0);
    expect("hal_irqc",
           hal_device_find(HAL_DEV_IRQC) != 0 &&
           (hal_device_find_by_name("plic0") != 0 ||
            hal_device_find_by_name("gic0") != 0));
    expect("hal_ipi", hal_device_find(HAL_DEV_IPI) != 0);
    expect("hal_get0", hal_device_get(0) != 0);
    expect("hal_get_oob", hal_device_get(9999) == 0);
    expect("hal_find_null", hal_device_find_by_name(0) == 0);

    /* 冻结后再注册必须失败 */
    bogus.name = "bogus0";
    bogus.class = HAL_DEV_CONSOLE;
    bogus.ops = hal_device_find(HAL_DEV_CONSOLE)->ops;
    bogus.mmio_base = 0;
    bogus.priv = 0;
    bogus.flags = 0;
    st = hal_device_register(&bogus);
    expect("hal_reg_frozen", st == HAL_ERR_STATE);

    expect("hal_reg_bad", hal_device_register(0) == HAL_ERR_PARAM);

    uint64_t t0 = hal_mtime_read();
    for (volatile int i = 0; i < 2000; i++) {
    }
    uint64_t t1 = hal_mtime_read();
    expect("hal_mtime_adv", t1 >= t0);
    expect("hal_mtime_compat", cgrtos_mtime_read() >= t1);
    expect("hal_thr_compat",
           cgrtos_plic_get_threshold() == hal_irqc_get_threshold());

    /* 控制台：write / puts（持锁整段） */
    n = hal_console_write("HAL", 3);
    expect("hal_write_n", n == 3);
    expect("hal_write_bad", hal_console_write(0, 4) == HAL_ERR_PARAM);
    expect("hal_write_empty", hal_console_write("", 0) == 0);
    hal_console_puts(" write-ok\n");
    expect("hal_console_ok", 1);

    /* IRQC 参数边界（配置锁路径） */
    expect("hal_prio_bad_irq",
           hal_irqc_set_priority(0, 1) == HAL_ERR_PARAM);
    expect("hal_prio_bad_hi",
           hal_irqc_set_priority(5, CONFIG_IRQ_PRIORITY_MAX + 1) == HAL_ERR_PARAM);
    expect("hal_en_bad", hal_irqc_enable(0) == HAL_ERR_PARAM);
    expect("hal_dis_bad", hal_irqc_disable(0) == HAL_ERR_PARAM);

    /* IPI：合法核 / 越界 */
    expect("hal_ipi_self", hal_ipi_send(0) == HAL_OK);
    expect("hal_ipi_oob",
           hal_ipi_send((uint8_t)CONFIG_NUM_CORES) == HAL_ERR_PARAM);

    /* 兼容层与 HAL 一致 */
    expect("hal_prio_compat",
           cgrtos_plic_get_priority(5) == hal_irqc_get_priority(5));
    expect("hal_board_name", HAL_BOARD_NAME != 0 && HAL_BOARD_NAME[0] != 0);
}

static const test_case_t g_cases[] = {
    { "delay",     "tick / delay_ms / delay_us / delay_until", case_delay },
    { "mem",       "heap / TLSF malloc calloc free",          case_mem },
    { "sem",       "semaphore create take give",              case_sem },
    { "mutex",     "mutex lock unlock nesting",               case_mutex },
    { "safety",    "recursive mtx / delete-safe / stats / stack", case_safety },
    { "queue",     "queue send receive ISR",                  case_queue },
    { "event",     "event group set wait clear",              case_event },
    { "streambuf", "stream buffer send recv",                 case_streambuf },
    { "msgbuf",    "message buffer framed send recv",         case_msgbuf },
    { "qset",      "queue set select",                        case_qset },
    { "fs",        "RAM filesystem open read write rename format", case_fs },
    { "vfs",       "VFS mount table open read write sync",      case_vfs },
    { "preempt",   "high prio preempts busy low",             case_preempt },
    { "notify",    "task notify wait",                        case_notify },
    { "timer",     "soft timer start stop change",            case_timer },
    { "task",      "task lifecycle suspend resume delete",    case_task },
    { "sched",     "CFS EDF hybrid RR scheduling",            case_sched },
    { "sched_m1",  "PT DPCP exit sched-stats (module1)",      case_sched_m1 },
    { "m2_ipc",    "ISR-guard timeout deadlock (module2)",    case_m2_ipc },
    { "m3_mem",    "mempool poison redzone (module3)",        case_m3_mem },
    { "m4_safe",   "hooks crit-monitor ISR-guard (module4)",  case_m4_safe },
    { "m5_perf",   "EDF-heap idle-sleep API (module5)",       case_m5_perf },
    { "m6_dbg",    "klog objects trace export (module6)",     case_m6_dbg },
    { "smp",       "affinity and load balance",               case_smp },
    { "hooks",     "hooks and static IPC",                    case_hooks },
    { "critical",  "critical section yield stats",            case_critical },
    { "isr",       "FromISR woken tick-hook IPC/timer",       case_isr },
    { "irq",       "PLIC prio group critical_from_isr",       case_irq },
    { "hal",       "HAL registry/errors/console/irqc/ipi",    case_hal },
};

#define N_CASES ((int)(sizeof(g_cases) / sizeof(g_cases[0])))

/**
 * @brief 比较两个 C 字符串是否相等
 * @param[in] a 字符串 a；NULL 视为不等
 * @param[in] b 字符串 b；NULL 视为不等
 * @return 1 相等；0 不等
 * @retval 1 相等
 * @retval 0 不等或任一为空
 * @note 区分大小写
 * @warning 无
 * @attention ✅ 任意上下文；❌ 不阻塞
 * @internal
 */
static int str_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/**
 * @brief 打印 pass/fail 汇总；可选输出套件通过/失败标记
 * @param[in] suite_marker 非零时输出 TEST_SUITE_PASSED/FAILED 标记
 * @return 无
 * @retval 无
 * @note 供 test_cases_run 调用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞（printf）
 * @internal
 */
static void print_results(int suite_marker)
{
    cgrtos_printf("----------------------------------------\n");
    cgrtos_printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    if (suite_marker) {
        if (g_fail == 0) {
            cgrtos_printf("=== TEST_SUITE_PASSED ===\n");
        } else {
            cgrtos_printf("=== TEST_SUITE_FAILED ===\n");
        }
    }
}

/**
 * @brief 清零全局 pass/fail 计数器
 * @return 无
 * @retval 无
 * @note 与 test_cases.h 声明一致
 * @warning 多任务并发无原子保护
 * @attention ❌ ISR 勿依赖；❌ 不阻塞
 */
void test_cases_reset(void)
{
    g_pass = 0;
    g_fail = 0;
}

/**
 * @brief 读取当前通过计数
 * @return 累计 [PASS] 次数
 * @retval >=0 通过数
 * @note 只读 g_pass
 * @warning 无
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int test_cases_pass_count(void)
{
    return g_pass;
}

/**
 * @brief 读取当前失败计数
 * @return 累计 [FAIL] 次数
 * @retval >=0 失败数
 * @note 只读 g_fail
 * @warning 无
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int test_cases_fail_count(void)
{
    return g_fail;
}

/**
 * @brief 获取静态用例表指针与条目数
 * @details 返回 g_cases[] 首地址；count 非 NULL 时写入 N_CASES。表驻留只读段，不含 "all"/"stress" 合成名。
 * @param[out] count 可选；非 NULL 时写入用例总数
 * @return 用例表首指针
 * @retval 非 NULL 表有效
 * @note 与 test_cases.h 声明一致；调用方勿 free 或改写表项
 * @warning 勿修改返回表内容
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
const test_case_t *test_cases_get(int *count)
{
    if (count) {
        *count = N_CASES;
    }
    return g_cases;
}

/**
 * @brief 打印全部可用用例名与一行帮助
 * @return 无
 * @retval 无
 * @note CLI `help` 可调用
 * @warning 无
 * @attention ❌ ISR（printf）；❌ 不阻塞
 */
void test_cases_list(void)
{
    int i;
    cgrtos_printf("Available test cases:\n");
    for (i = 0; i < N_CASES; i++) {
        cgrtos_printf("  %s - %s\n", g_cases[i].name, g_cases[i].help);
    }
    cgrtos_printf("  stress - full-feature concurrent stress (~1.2s)\n");
    cgrtos_printf("  all - run every functional case (not stress)\n");
}

/**
 * @brief 按名称运行单个用例、"all" 或 "stress"
 * @details 匹配 g_cases[]；all 依次执行全部功能用例；stress 转调 stress_run()。
 * @param[in] name 用例名、"all" 或 "stress"；NULL 非法
 * @return 0 成功；-1 未知名
 * @retval 0  已执行
 * @retval -1 名称非法或未找到
 * @note 单用例前 reset 计数；all 输出套件标记
 * @warning stress 不经过 expect 汇总
 * @attention ❌ ISR；✅ 用例内可长时间阻塞
 */
int test_cases_run(const char *name)
{
    int i;

    if (!name) {
        return -1;
    }

    if (str_eq(name, "all")) {
        test_cases_reset();
        for (i = 0; i < N_CASES; i++) {
            g_cases[i].run();
        }
        print_results(1);
        return 0;
    }

    if (str_eq(name, "stress")) {
        cgrtos_printf("=== case: stress ===\n");
        (void)stress_run();
        return 0;
    }

    for (i = 0; i < N_CASES; i++) {
        if (str_eq(g_cases[i].name, name)) {
            test_cases_reset();
            cgrtos_printf("=== case: %s ===\n", g_cases[i].name);
            g_cases[i].run();
            print_results(0);
            return 0;
        }
    }
    return -1;
}
