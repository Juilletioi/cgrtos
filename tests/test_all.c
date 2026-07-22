/**
 * @file test_all.c
 * @brief APP=test 入口：启动完整功能测试套件
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 脚本识别标记：`[PASS]` / `[FAIL]`、`=== TEST_SUITE_PASSED ===` /
 * `=== TEST_SUITE_FAILED ===`。
 */

#include "../kernel/cgrtos.h"
#include "test_cases.h"

/**
 * @brief 测试驱动任务：运行全部用例后空转
 * @details 调用 test_cases_run("all")，然后每秒 delay 保持存活。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 亲和性由 main 绑到核 0，避免被负载均衡迁走
 * @warning 套件失败不复位板卡，仅打印标记
 * @attention ❌ ISR；✅ 内部大量阻塞 API
 * @internal
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

/**
 * @brief hart0 测试应用入口
 * @details cgrtos_init → 创建 tester 任务并绑核 0 → cgrtos_start。
 * @param[in] hartid 核号
 * @param[in] fdt    设备树（忽略）
 * @param[in] end    DDR/链接末地址提示
 * @return 正常不返回；异常路径可能返回 0
 * @retval 0 仅异常
 * @note 任务 ID 假定 create 后为 1（首个用户任务），用于 set_affinity
 * @warning 若 create 失败仍 start，套件不会运行
 * @attention ❌ ISR；✅ 启动调度
 */
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

/**
 * @brief 次核入口
 * @details 转调 cgrtos_start_secondary。
 * @param[in] hartid 次核编号
 * @return 无（不返回）
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 进入调度
 */
void secondary_main(int hartid)
{
    /* Print after online flag is published (see cgrtos_start_secondary). */
    cgrtos_start_secondary(hartid);
}
