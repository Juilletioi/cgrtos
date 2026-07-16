/**
 * @file stress_cases.c
 * @brief CG-RTOS 全 feature 并发压力测试实现
 *
 * 同时压测：SMP / RR·CFS·EDF·Hybrid·Priority / sem·mutex·queue·event·notify /
 * 软定时器 / TLSF / 延时超时 / 亲和与负载均衡 / 挂起恢复删除 / 临界区。
 *
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

static void bump_core(void)
{
    if ((uint8_t)read_csr(mhartid) == 0) {
        __atomic_fetch_add(&g_core0_hits, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&g_core1_hits, 1, __ATOMIC_RELAXED);
    }
}

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

static void timer_cb(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&g_tmr_fires, 1, __ATOMIC_RELAXED);
}

static void nop_worker(void *arg)
{
    (void)arg;
    while (!g_stop) {
        bump_core();
        cgrtos_delay(4);
    }
    wait_until_stop();
}

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
 * @brief 运行一轮完整压力测试。
 * @return 0 通过；非 0 有失败。
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
    cgrtos_printf("  duration=%d ms  secondary=%u\n",
                  STRESS_MS, g_secondary_online);

    expect("boot_secondary", g_secondary_online != 0);
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
    uint32_t cs0_c1 = g_cs_count_core[1];
    unsigned long heap0 = cgrtos_get_free_heap();

    tick_t t0 = cgrtos_get_ticks();
    while ((cgrtos_get_ticks() - t0) < (tick_t)STRESS_MS) {
        cgrtos_task_yield();
        cgrtos_sched_load_balance();
        /* ISR-style ops from task context (APIs are critical-section safe). */
        if (g_sem[0]) {
            cgrtos_sem_give_from_isr(g_sem[0]);
        }
        if (g_eg) {
            cgrtos_event_group_set_from_isr(g_eg, 0x1);
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
    uint32_t d1 = g_cs_count_core[1] - cs0_c1;
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
    cgrtos_printf("  LB migrate_total=%u secondary=%u\n",
                  g_lb_migrate_count, g_secondary_online);

    expect("stress_rr_progress", g_rr_ops > 50);
    expect("stress_cfs_progress", g_cfs_ops > 50);
    expect("stress_edf_progress", (g_edf_ok + g_edf_miss) > 10);
    expect("stress_edf_ok_ratio", g_edf_ok + 5 >= g_edf_miss);
    expect("stress_hyb_progress", g_hyb_ops > 20);
    expect("stress_sem_progress", g_sem_ops > 20);
    expect("stress_q_progress", g_q_ops > 20);
    expect("stress_mtx_progress", g_mtx_ops > 10);
    expect("stress_evt_progress", g_evt_ops > 10);
    expect("stress_notify_progress", g_notify_ops > 10);
    expect("stress_heap_progress", g_heap_ops > 50);
    expect("stress_tmr_progress", g_tmr_fires > 5);
    expect("stress_life_progress", g_life_ops > 3);
    expect("stress_yield_progress", g_yield_storm > 50);
    expect("stress_both_cores", g_core0_hits > 50 && g_core1_hits > 20);
    expect("stress_cs_both", d0 > 50 && d1 > 20);
    expect("stress_secondary_alive", g_secondary_online != 0);
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

int stress_pass_count(void)
{
    return g_pass;
}

int stress_fail_count(void)
{
    return g_fail;
}
