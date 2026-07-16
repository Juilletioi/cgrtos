#include "../kernel/cgrtos.h"

/*
 * Microbenchmarks for CG-RTOS.
 * Markers for scripts/run_qemu.sh:
 *   BENCH name=value
 *   === BENCH_DONE ===
 */

static cgrtos_sem_t *g_ping;
static cgrtos_sem_t *g_pong;
static volatile int g_peer_done;

static uint64_t cycles_now(void)
{
    return cgrtos_mtime_read();
}

static void print_bench(const char *name, uint64_t total_us, uint64_t iters)
{
    uint64_t per = iters ? (total_us * 1000ULL) / iters : 0; /* ns */
    cgrtos_printf("BENCH %s=%lu ns (total_us=%lu iters=%lu)\n",
                  name, (unsigned long)per, (unsigned long)total_us,
                  (unsigned long)iters);
}

static void yield_peer(void *arg)
{
    (void)arg;
    while (!g_peer_done) {
        cgrtos_task_yield();
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

static void sem_peer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200; i++) {
        cgrtos_sem_take(g_ping, portMAX_DELAY);
        cgrtos_sem_give(g_pong);
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

static void run_bench(void *arg)
{
    (void)arg;
    cgrtos_printf("\n======== CG-RTOS Microbenchmarks ========\n");

    /* Context switch: yield against a same-prio peer on core 0 */
    {
        g_peer_done = 0;
        task_id_t self = g_current[0] ? g_current[0]->id : 0;
        task_id_t peer = cgrtos_task_create("yp", yield_peer, 0, 10, SCHED_RR);
        if (self) {
            cgrtos_task_set_affinity(self, 0);
        }
        cgrtos_task_set_affinity(peer, 0);
        cgrtos_delay_ms(5);

        const int iters = 1000;
        uint64_t t0 = cycles_now();
        for (int i = 0; i < iters; i++) {
            cgrtos_task_yield();
        }
        uint64_t t1 = cycles_now();
        g_peer_done = 1;
        cgrtos_delay_ms(10);
        cgrtos_task_delete(peer);
        print_bench("context_switch", t1 - t0, (uint64_t)iters);
    }

    /* Semaphore ping-pong */
    {
        g_ping = cgrtos_sem_create_binary();
        g_pong = cgrtos_sem_create_binary();
        task_id_t peer = cgrtos_task_create("sp", sem_peer, 0, 9, SCHED_PRIORITY);
        cgrtos_delay_ms(2);
        const int iters = 200;
        uint64_t t0 = cycles_now();
        for (int i = 0; i < iters; i++) {
            cgrtos_sem_give(g_ping);
            cgrtos_sem_take(g_pong, portMAX_DELAY);
        }
        uint64_t t1 = cycles_now();
        cgrtos_task_delete(peer);
        print_bench("sem_pingpong", t1 - t0, (uint64_t)iters);
        cgrtos_sem_delete(g_ping);
        cgrtos_sem_delete(g_pong);
    }

    /* Queue round-trip */
    {
        cgrtos_queue_t *q = cgrtos_queue_create(8, sizeof(uint32_t));
        const int iters = 500;
        uint64_t t0 = cycles_now();
        for (int i = 0; i < iters; i++) {
            uint32_t v = (uint32_t)i, out = 0;
            cgrtos_queue_send(q, &v, 0);
            cgrtos_queue_receive(q, &out, 0);
        }
        uint64_t t1 = cycles_now();
        print_bench("queue_roundtrip", t1 - t0, (uint64_t)iters);
        cgrtos_queue_delete(q);
    }

    /* TLSF malloc/free */
    {
        const int iters = 400;
        uint64_t t0 = cycles_now();
        for (int i = 0; i < iters; i++) {
            void *p = cgrtos_malloc(64 + (unsigned)(i & 63));
            cgrtos_free(p);
        }
        uint64_t t1 = cycles_now();
        print_bench("malloc_free_small", t1 - t0, (uint64_t)iters);

        t0 = cycles_now();
        for (int i = 0; i < 100; i++) {
            void *p = cgrtos_malloc(2048);
            cgrtos_free(p);
        }
        t1 = cycles_now();
        print_bench("malloc_free_2k", t1 - t0, 100ULL);

        void *hole[32];
        for (int i = 0; i < 32; i++) {
            hole[i] = cgrtos_malloc(128);
        }
        for (int i = 0; i < 32; i += 2) {
            cgrtos_free(hole[i]);
            hole[i] = 0;
        }
        t0 = cycles_now();
        for (int i = 0; i < 100; i++) {
            void *p = cgrtos_malloc(96);
            cgrtos_free(p);
        }
        t1 = cycles_now();
        print_bench("malloc_free_frag", t1 - t0, 100ULL);
        for (int i = 1; i < 32; i += 2) {
            cgrtos_free(hole[i]);
        }
    }

    cgrtos_printf("BENCH cs_total=%u count\n", g_cs_count);
    cgrtos_printf("BENCH cs_core0=%u count\n", g_cs_count_core[0]);
    cgrtos_printf("BENCH cs_core1=%u count\n", g_cs_count_core[1]);
    cgrtos_printf("BENCH ready_core0=%u tasks\n", cgrtos_sched_ready_count(0));
    cgrtos_printf("BENCH ready_core1=%u tasks\n", cgrtos_sched_ready_count(1));

    cgrtos_printf("=== BENCH_DONE ===\n");
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

int main(int hartid, void *fdt, void *end)
{
    (void)fdt;
    (void)end;
    cgrtos_init();
    cgrtos_printf("  [BOOT] Hart %d DDR end=%p (bench)\n", hartid, end);
    cgrtos_task_create("bench", run_bench, 0, 10, SCHED_PRIORITY);
    cgrtos_start();
    return 0;
}

void secondary_main(int hartid)
{
    cgrtos_start_secondary(hartid);
}
