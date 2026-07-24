/**
 * @file bench_all.c
 * @brief APP=bench 入口：CG-RTOS 微基准测试套件
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * 脚本识别标记：`BENCH name=value`、`=== BENCH_DONE ===`。
 * 测量上下文切换、信号量 ping-pong、队列往返、TLSF 分配及调度统计。
 */

#include "../kernel/cgrtos.h"

static cgrtos_sem_t *g_ping;
static cgrtos_sem_t *g_pong;
static volatile int g_peer_done;

/**
 * @brief 读取当前机器周期计数
 * @details 封装 cgrtos_mtime_read，作为基准计时源。
 * @return 当前 mtime 周期值
 * @retval >=0 单调递增的周期计数
 * @note 用于前后差分计算耗时
 * @warning 跨核读 mtime 时语义取决于 HAL 实现
 * @attention ✅ 任务上下文（非 ISR 限制）；❌ block/switch
 * @internal
 */
static uint64_t cycles_now(void)
{
    return cgrtos_mtime_read();
}

/**
 * @brief 打印单项基准结果
 * @details 将总微秒与迭代次数换算为每次纳秒并输出 `BENCH name=...` 行。
 * @param[in] name     基准项名称
 * @param[in] total_us 总耗时（微秒）
 * @param[in] iters    迭代次数；为 0 时 per 输出 0
 * @return 无
 * @retval 无
 * @note 供 scripts/run_qemu.sh 解析
 * @warning iters 为 0 时不做除零，per 固定为 0
 * @attention ❌ ISR；✅ 可阻塞 printf
 * @internal
 */
static void print_bench(const char *name, uint64_t total_us, uint64_t iters)
{
    uint64_t per = iters ? (total_us * 1000ULL) / iters : 0; /* ns */
    cgrtos_printf("BENCH %s=%lu ns (total_us=%lu iters=%lu)\n",
                  name, (unsigned long)per, (unsigned long)total_us,
                  (unsigned long)iters);
}

/**
 * @brief 上下文切换基准的对端任务
 * @details 在 g_peer_done 置位前持续 yield，供主测任务反复切换。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 与 run_bench 中 yield 环同核、同优先级
 * @warning 删除前须置 g_peer_done，否则对端空转
 * @attention ❌ ISR；✅ 阻塞 yield/delay
 * @internal
 */
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

/**
 * @brief 信号量 ping-pong 基准的对端任务
 * @details 循环 200 次：take(g_ping) → give(g_pong)，与主测任务对打。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 优先级 9，低于 bench 主测任务
 * @warning 依赖全局 g_ping/g_pong 已创建
 * @attention ❌ ISR；✅ 阻塞 sem_take
 * @internal
 */
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

/**
 * @brief 微基准驱动任务：依次运行各项测量
 * @details 执行 context_switch、sem_pingpong、queue_roundtrip、malloc 系列及调度统计，最后打印 BENCH_DONE。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 各项测试串行；peer 任务在段内创建/删除
 * @warning 失败不中止后续项；部分项依赖核 0 亲和性
 * @attention ❌ ISR；✅ 大量阻塞 API 与 task_create/delete
 * @internal
 */
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

/**
 * @brief hart0 基准应用入口
 * @details cgrtos_init → 创建 bench 任务 → cgrtos_start。
 * @param[in] hartid 核号
 * @param[in] fdt    设备树（忽略）
 * @param[in] end    DDR/链接末地址提示
 * @return 正常不返回；异常路径可能返回 0
 * @retval 0 仅异常
 * @note 不绑核；run_bench 内自行设置亲和性
 * @warning 若 task_create 失败仍 start，基准不会运行
 * @attention ❌ ISR；✅ block/switch（启动调度）
 */
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

/**
 * @brief 次核入口
 * @details 转调 cgrtos_start_secondary，参与 SMP 调度。
 * @param[in] hartid 次核编号
 * @return 无（不返回）
 * @retval 无
 * @note 基准主逻辑在 hart0 bench 任务
 * @warning 无
 * @attention ❌ ISR；✅ block/switch（进入调度）
 */
void secondary_main(int hartid)
{
    cgrtos_start_secondary(hartid);
}
