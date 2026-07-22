/**
 * @file mpu.c
 * @brief MPU / 任务地址空间隔离预留接口（模块 4）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 */

#include "cgrtos.h"

#if CONFIG_USE_MPU

/** @brief 已配置的 MPU 区域描述表 */
static cgrtos_mpu_region_t g_mpu_regions[CONFIG_MPU_MAX_REGIONS];
/** @brief 区域槽位占用标志 */
static uint8_t g_mpu_used[CONFIG_MPU_MAX_REGIONS];
/** @brief 各任务地址空间绑定表 */
static task_id_t g_mpu_task_as[CONFIG_MAX_TASKS];

/**
 * @brief 初始化 MPU 子系统（软件桩）
 * @details 清零区域表与任务绑定表，输出就绪日志。当前无硬件 MPU 操作，不阻塞、不切换。
 * @return pdPASS 始终成功
 * @retval pdPASS 初始化完成
 * @note 硬件移植时应在 arch 层填充寄存器配置
 * @warning 当前为桩实现，不提供真实内存保护
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_mpu_init(void)
{
    uint32_t i;
    for (i = 0; i < CONFIG_MPU_MAX_REGIONS; i++) {
        g_mpu_used[i] = 0;
        g_mpu_regions[i].base = 0;
        g_mpu_regions[i].size = 0;
        g_mpu_regions[i].attr = 0;
    }
    for (i = 0; i < CONFIG_MAX_TASKS; i++) {
        g_mpu_task_as[i] = 0;
    }
    CGRTOS_LOGI("mpu", "MPU stubs ready (no HW)");
    return pdPASS;
}

/**
 * @brief 配置指定 MPU 区域
 * @details 将区域描述写入 g_mpu_regions[idx] 并标记占用。软件记录，不写硬件寄存器。
 * @param[in] idx 区域索引（0 .. CONFIG_MPU_MAX_REGIONS-1）
 * @param[in] r   区域描述（base/size/attr）
 * @return pdPASS 成功；errPARAM 参数无效
 * @retval pdPASS   配置已保存
 * @retval errPARAM idx 越界或 r 为 NULL
 * @note 须在任务切换前完成区域配置
 * @warning 当前桩不生效于硬件
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_mpu_configure_region(uint32_t idx, const cgrtos_mpu_region_t *r)
{
    if (!r || idx >= CONFIG_MPU_MAX_REGIONS) {
        return errPARAM;
    }
    g_mpu_regions[idx] = *r;
    g_mpu_used[idx] = 1;
    return pdPASS;
}

/**
 * @brief 为任务启用地址空间隔离
 * @details 记录任务 id 到 g_mpu_task_as；硬件实现应在此加载 PMP/MPU 寄存器。不阻塞、不切换。
 * @param[in] id 任务 ID（须 1 .. CONFIG_MAX_TASKS-1）
 * @return pdPASS 成功；errPARAM id 无效
 * @retval pdPASS   隔离标记已设置
 * @retval errPARAM id 为 0 或越界
 * @note 实际硬件加载应在上下文切换时完成
 * @warning 当前桩不阻止非法内存访问
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_mpu_enable_task_isolation(task_id_t id)
{
    if (id == 0 || id >= CONFIG_MAX_TASKS) {
        return errPARAM;
    }
    g_mpu_task_as[id] = id;
    /* HW：此处应写 PMP/MPU 寄存器加载该任务区域集 */
    return pdPASS;
}

/**
 * @brief 为任务禁用地址空间隔离
 * @details 清除 g_mpu_task_as[id]。硬件实现应恢复默认映射。不阻塞、不切换。
 * @param[in] id 任务 ID（须 1 .. CONFIG_MAX_TASKS-1）
 * @return pdPASS 成功；errPARAM id 无效
 * @retval pdPASS   隔离标记已清除
 * @retval errPARAM id 为 0 或越界
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_mpu_disable_task_isolation(task_id_t id)
{
    if (id == 0 || id >= CONFIG_MAX_TASKS) {
        return errPARAM;
    }
    g_mpu_task_as[id] = 0;
    return pdPASS;
}

#else

/**
 * @brief 初始化 MPU 子系统（桩：MPU 未启用）
 * @details CONFIG_USE_MPU 关闭时直接返回 pdFAIL。不阻塞、不切换。
 * @return pdFAIL MPU 功能未编译
 * @retval pdFAIL 功能不可用
 * @note CONFIG_USE_MPU 须置 1 以启用真实实现
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_mpu_init(void) { return pdFAIL; }

/**
 * @brief 配置指定 MPU 区域（桩：MPU 未启用）
 * @details 忽略参数，返回 pdFAIL。不阻塞、不切换。
 * @param[in] idx 被忽略的索引
 * @param[in] r   被忽略的区域描述
 * @return pdFAIL MPU 功能未编译
 * @retval pdFAIL 功能不可用
 * @note CONFIG_USE_MPU 须置 1 以启用真实实现
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_mpu_configure_region(uint32_t idx, const cgrtos_mpu_region_t *r)
{
    (void)idx;
    (void)r;
    return pdFAIL;
}

/**
 * @brief 为任务启用地址空间隔离（桩：MPU 未启用）
 * @details 忽略参数，返回 pdFAIL。不阻塞、不切换。
 * @param[in] id 被忽略的任务 ID
 * @return pdFAIL MPU 功能未编译
 * @retval pdFAIL 功能不可用
 * @note CONFIG_USE_MPU 须置 1 以启用真实实现
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_mpu_enable_task_isolation(task_id_t id)
{
    (void)id;
    return pdFAIL;
}

/**
 * @brief 为任务禁用地址空间隔离（桩：MPU 未启用）
 * @details 忽略参数，返回 pdFAIL。不阻塞、不切换。
 * @param[in] id 被忽略的任务 ID
 * @return pdFAIL MPU 功能未编译
 * @retval pdFAIL 功能不可用
 * @note CONFIG_USE_MPU 须置 1 以启用真实实现
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_mpu_disable_task_isolation(task_id_t id)
{
    (void)id;
    return pdFAIL;
}

#endif
