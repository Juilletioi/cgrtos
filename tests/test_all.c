#include "../kernel/cgrtos.h"
#include "test_cases.h"

/*
 * Full-feature self-test for CG-RTOS.
 * Markers consumed by scripts/run_qemu.sh:
 *   [PASS] / [FAIL]
 *   === TEST_SUITE_PASSED === / === TEST_SUITE_FAILED ===
 */

static void run_tests(void *arg)
{
    (void)arg;
    cgrtos_printf("\n======== CG-RTOS Feature Test Suite ========\n");
    test_cases_run("all");
    while (1) {
        cgrtos_delay_ms(1000);
    }
}

int main(int hartid, void *fdt, void *end)
{
    (void)fdt;
    (void)end;

    cgrtos_init();
    cgrtos_printf("  [BOOT] Hart %d DDR end=%p\n", hartid, end);

    cgrtos_task_create("tester", run_tests, 0, 10, SCHED_PRIORITY);
    /* Keep the suite driver pinned so load-balance cannot exile it. */
    cgrtos_task_set_affinity(1, 0);
    cgrtos_start();
    return 0;
}

void secondary_main(int hartid)
{
    /* Print after online flag is published (see cgrtos_start_secondary). */
    cgrtos_start_secondary(hartid);
}
