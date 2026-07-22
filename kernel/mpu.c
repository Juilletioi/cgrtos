/**
 * @file mpu.c
 * @brief MPU / 任务地址空间隔离预留接口（模块4）
 * @details 当前为软件桩；适配带 MPU 的硬件时在 arch 层填充实现。
 */
#include "cgrtos.h"

#if CONFIG_USE_MPU

static cgrtos_mpu_region_t g_mpu_regions[CONFIG_MPU_MAX_REGIONS];
static uint8_t g_mpu_used[CONFIG_MPU_MAX_REGIONS];
static task_id_t g_mpu_task_as[CONFIG_MAX_TASKS];

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

int cgrtos_mpu_configure_region(uint32_t idx, const cgrtos_mpu_region_t *r)
{
    if (!r || idx >= CONFIG_MPU_MAX_REGIONS) {
        return errPARAM;
    }
    g_mpu_regions[idx] = *r;
    g_mpu_used[idx] = 1;
    return pdPASS;
}

int cgrtos_mpu_enable_task_isolation(task_id_t id)
{
    if (id == 0 || id >= CONFIG_MAX_TASKS) {
        return errPARAM;
    }
    g_mpu_task_as[id] = id;
    /* HW：此处应写 PMP/MPU 寄存器加载该任务区域集 */
    return pdPASS;
}

int cgrtos_mpu_disable_task_isolation(task_id_t id)
{
    if (id == 0 || id >= CONFIG_MAX_TASKS) {
        return errPARAM;
    }
    g_mpu_task_as[id] = 0;
    return pdPASS;
}

#else

int cgrtos_mpu_init(void) { return pdFAIL; }
int cgrtos_mpu_configure_region(uint32_t idx, const cgrtos_mpu_region_t *r)
{
    (void)idx;
    (void)r;
    return pdFAIL;
}
int cgrtos_mpu_enable_task_isolation(task_id_t id)
{
    (void)id;
    return pdFAIL;
}
int cgrtos_mpu_disable_task_isolation(task_id_t id)
{
    (void)id;
    return pdFAIL;
}

#endif
