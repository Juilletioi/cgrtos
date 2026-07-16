/**
 * @file stress_all.c
 * @brief APP=stress 入口：启动后自动跑一轮压力测试
 *
 * 脚本识别：=== STRESS_PASSED === / === STRESS_FAILED ===
 */
#include "../kernel/cgrtos.h"
#include "stress_cases.h"

static void stress_task(void *arg)
{
    (void)arg;
    int rc = stress_run();
    if (rc == 0) {
        cgrtos_printf("=== TEST_SUITE_PASSED ===\n");
    } else {
        cgrtos_printf("=== TEST_SUITE_FAILED ===\n");
    }
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

int main(int hartid, void *fdt, void *end)
{
    (void)fdt;
    (void)end;

    cgrtos_init();
    cgrtos_printf("  [BOOT] Hart %d DDR end=%p (stress)\n", hartid, end);
    cgrtos_task_create("stress", stress_task, 0, 10, SCHED_PRIORITY);
    cgrtos_task_set_affinity(1, 0);
    cgrtos_start();
    return 0;
}

void secondary_main(int hartid)
{
    cgrtos_start_secondary(hartid);
}
