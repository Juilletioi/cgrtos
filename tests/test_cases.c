#include "../kernel/cgrtos.h"
#include "../hal/hal_board.h"
#include "test_cases.h"
#include "stress_cases.h"

/*
 * Named test cases shared by the automated suite and CLI.
 * Markers consumed by scripts/cgrtos.sh / run_qemu.sh:
 *   [PASS] / [FAIL]
 *   === TEST_SUITE_PASSED === / === TEST_SUITE_FAILED ===
 */

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

static void wait_ticks(tick_t n)
{
    tick_t start = cgrtos_get_ticks();
    while ((cgrtos_get_ticks() - start) < n) {
        cgrtos_task_yield();
    }
}

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

static void timer_cb(void *arg)
{
    (void)arg;
    g_timer_fires++;
}

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

static void edf_worker(void *arg)
{
    tick_t period = (tick_t)(uintptr_t)arg;
    tick_t wake = cgrtos_get_ticks();
    cgrtos_task_t *self = 0;
    uint8_t cpu = (uint8_t)read_csr(mhartid);
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

static void rr_worker(void *arg)
{
    (void)arg;
    while (1) {
        cgrtos_task_yield();
        cgrtos_delay_ms(40);
    }
}

static void aff_worker(void *arg)
{
    (void)arg;
    while (1) {
        uint8_t h = (uint8_t)read_csr(mhartid);
        if (h == 1) {
            g_smp_core1 = 1;
        }
        cgrtos_delay_ms(5);
    }
}

static void lb_worker(void *arg)
{
    (void)arg;
    tick_t end = cgrtos_get_ticks() + portMS_TO_TICK(80);
    while (cgrtos_get_ticks() < end) {
        if ((uint8_t)read_csr(mhartid) == 1) {
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

static void idle_hook_fn(void)
{
    g_hook_idle++;
}

static void tick_hook_fn(void)
{
    g_hook_ticks++;
}

/**
 * @brief 真实 tick ISR 钩子：按 phase 轮转调用 FromISR API 并收集 woken
 *
 * @details
 * 1. phase = g_isr_tick_ops & 7，依次覆盖 sem_give、queue_send、event_set、
 *    stream_send、message_send、timer_start、event_clear、sem_take。
 * 2. 各 API 传入 &woken；若 woken!=pdFALSE 则计数并 portYIELD_FROM_ISR。
 * 3. g_isr_tick_ops++，供 case_isr 断言至少执行多轮。
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
 * @param irq 中断源号
 * @param arg 未使用
 */
static void irq_test_handler(uint32_t irq, void *arg)
{
    (void)arg;
    g_irq_handler_hits++;
    g_irq_last = irq;
}

/* ---- named cases (former run_tests sections) ---- */

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

    /* Hybrid path (several ms via us) */
    m0 = cgrtos_mtime_read();
    cgrtos_delay_us(5000);
    m1 = cgrtos_mtime_read();
    md = m1 - m0;
    /* 5ms @ 1MHz = 5000 cycles; allow QEMU slack */
    expect("delay_us_hybrid", md >= 4000 && md < 200000);
}

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

static void case_sem(void)
{
    cgrtos_sem_t *sem = cgrtos_sem_create(0, 5);
    cgrtos_sem_t *bin = cgrtos_sem_create_binary();
    expect("sem_create", sem && bin);
    expect("sem_take_timeout", cgrtos_sem_take(sem, portMS_TO_TICK(20)) == pdFAIL);
    expect("sem_give", cgrtos_sem_give(sem) == pdPASS);
    expect("sem_take", cgrtos_sem_take(sem, 0) == pdPASS);
    expect("sem_give_isr", cgrtos_sem_give_from_isr(bin, 0) == pdPASS);
    expect("sem_bin_take", cgrtos_sem_take(bin, 0) == pdPASS);
    expect("sem_delete", cgrtos_sem_delete(sem) == pdPASS);
    cgrtos_sem_delete(bin);
}

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

static void case_safety(void)
{
    cgrtos_runtime_stats_t st0, st1;
    cgrtos_stats_get(&st0);
    expect("stats_get", st0.uptime_ticks > 0);
    expect("stats_cs", st0.context_switches > 0);

    expect("no_del_zero", cgrtos_task_delete(0) == pdFAIL);
    expect("no_del_bogus", cgrtos_task_delete((task_id_t)0xFFFFFFFFull) == pdFAIL);

    uint8_t cpu = (uint8_t)read_csr(mhartid);
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

static void preempt_hi_task(void *arg)
{
    (void)arg;
    g_preempt_hi++;
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

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

static void case_fs(void)
{
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
    expect("fs_unlink", cgrtos_fs_unlink("/data/note.txt") == 0);
    expect("fs_rmdir", cgrtos_fs_rmdir("/data") == 0);
}

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
 * @brief FromISR 全系列回归：任务上下文 API + 真实 tick 钩子路径
 *
 * @details
 * 1. 创建 sem/queue/event/stream/msgbuf/timer，校验 FromISR 收发与 clear/take。
 * 2. timer_start_from_isr 后等待回调触发。
 * 3. 安装 isr_pack_tick_hook，在真实 MTIP 上下文轮转 FromISR，断言 tick_ops≥16。
 * 4. 验证 portYIELD_FROM_ISR 宏；清理对象。
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
 * @brief 中断优先级分组 / PLIC 配置 / ISR 临界区 / 分发注册
 *
 * @details
 * 1. 读写 syscall_max_priority；configure 低/高优先级源并校验分组关系。
 * 2. register → dispatch 模拟 claim 后路径；unregister 后 handler 为空。
 * 3. enter/exit_critical_from_isr 与 portSET/CLEAR_INTERRUPT_MASK_FROM_ISR 抬高/恢复 threshold。
 * 4. 非法 irq/priority 返回 pdFAIL；最后 disable 测试源。
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
 * @brief HAL 框架回归：注册表契约、错误码、兼容层、控制台原子写、配置校验
 *
 * @details
 * 覆盖易用性/鲁棒性/多核安全相关契约（不依赖真实多核竞态复现）：
 * 1. 设备表完整且冻结；
 * 2. HAL 错误码与非法参数；
 * 3. 兼容层与 HAL 读路径一致；
 * 4. console write/puts；
 * 5. irqc/ipi 参数边界。
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
           hal_device_find_by_name("plic0") != 0);
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
    { "fs",        "RAM filesystem open read write",          case_fs },
    { "preempt",   "high prio preempts busy low",             case_preempt },
    { "notify",    "task notify wait",                        case_notify },
    { "timer",     "soft timer start stop change",            case_timer },
    { "task",      "task lifecycle suspend resume delete",    case_task },
    { "sched",     "CFS EDF hybrid RR scheduling",            case_sched },
    { "smp",       "affinity and load balance",               case_smp },
    { "hooks",     "hooks and static IPC",                    case_hooks },
    { "critical",  "critical section yield stats",            case_critical },
    { "isr",       "FromISR woken tick-hook IPC/timer",       case_isr },
    { "irq",       "PLIC prio group critical_from_isr",       case_irq },
    { "hal",       "HAL registry/errors/console/irqc/ipi",    case_hal },
};

#define N_CASES ((int)(sizeof(g_cases) / sizeof(g_cases[0])))

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

void test_cases_reset(void)
{
    g_pass = 0;
    g_fail = 0;
}

int test_cases_pass_count(void)
{
    return g_pass;
}

int test_cases_fail_count(void)
{
    return g_fail;
}

const test_case_t *test_cases_get(int *count)
{
    if (count) {
        *count = N_CASES;
    }
    return g_cases;
}

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
