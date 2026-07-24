/**
 * @file stress_all.c
 * @brief APP=stress 入口：启动后自动跑一轮压力测试
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * 脚本识别：`=== TEST_SUITE_PASSED ===` / `=== TEST_SUITE_FAILED ===`
 * （stress_run 内部另打印 STRESS_PASSED / STRESS_FAILED）。
 */

#include "../kernel/cgrtos.h"
#include "stress_cases.h"

/**
 * @brief 压力测试驱动任务
 * @details 调用 stress_run()，按返回值打印套件通过/失败标记后空转。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 亲和性由 main 绑到核 0
 * @warning 失败不复位板卡，仅打印标记
 * @attention ❌ ISR；✅ 内部大量阻塞 API
 * @internal
 */
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

/**
 * @brief hart0 压力测试应用入口
 * @details cgrtos_init → 创建 stress 任务并绑核 0 → cgrtos_start。
 * @param[in] hartid 核号
 * @param[in] fdt    设备树（忽略）
 * @param[in] end    DDR/链接末地址提示
 * @return 正常不返回；异常路径可能返回 0
 * @retval 0 仅异常
 * @note 任务 ID 假定 create 后为 1，用于 set_affinity
 * @warning 若 create 失败仍 start，套件不会运行
 * @attention ❌ ISR；✅ block/switch（启动调度）
 */
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

/**
 * @brief 次核入口
 * @details 转调 cgrtos_start_secondary。
 * @param[in] hartid 次核编号
 * @return 无（不返回）
 * @retval 无
 * @note 压力主逻辑在 hart0 stress 任务
 * @warning 无
 * @attention ❌ ISR；✅ block/switch（进入调度）
 */
void secondary_main(int hartid)
{
    cgrtos_start_secondary(hartid);
}
