/**
 * @file mempool.c
 * @brief 固定块静态内存池（模块 3）：无碎片、确定时延
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 */

#include "cgrtos.h"
#include <string.h>

#ifndef CGRTOS_MAX_MEMPOOL
#define CGRTOS_MAX_MEMPOOL 8
#endif

/** @brief 内存池控制块（内部） */
typedef struct {
    uint8_t  in_use;
    uint8_t *base;
    uint32_t block_size;
    uint32_t block_count;
    uint32_t free_count;
    uint32_t *free_stack; /* indices of free blocks */
    uint32_t free_top;
    uint32_t magic;
} mempool_ctrl_t;

#define MEMPOOL_MAGIC 0x4D454D50u /* 'MEMP' */

/** @brief 全局内存池槽位表 */
static mempool_ctrl_t g_pools[CGRTOS_MAX_MEMPOOL];

/**
 * @brief 创建固定块内存池
 * @details 在全局槽位中分配控制块，块大小 8 字节对齐，初始化空闲索引栈。全程持临界区，不切换任务；内部调用 cgrtos_malloc 分配栈数组。
 * @param[in] storage     用户提供的块存储区（连续 block_count 块）
 * @param[in] block_size  单块净荷字节数（须 >= sizeof(void*)）
 * @param[in] block_count 块数量（须 > 0）
 * @return 内存池句柄；失败返回 NULL
 * @retval 非 NULL 有效池句柄
 * @retval NULL    参数无效、槽位已满或 malloc 失败
 * @note storage 生命周期须覆盖池的整个使用期
 * @warning 不可在 ISR 中调用（含 cgrtos_malloc）
 * @attention ❌ ISR；❌ block/switch
 */
cgrtos_mempool_t *cgrtos_mempool_create(void *storage, uint32_t block_size,
                                        uint32_t block_count)
{
    mempool_ctrl_t *p;
    uint32_t i;
    uint32_t *stack;

    if (!storage || block_size < sizeof(void *) || block_count == 0) {
        return 0;
    }
    /* 对齐块大小 */
    block_size = (block_size + 7U) & ~7U;

    cgrtos_enter_critical();
    p = 0;
    for (i = 0; i < CGRTOS_MAX_MEMPOOL; i++) {
        if (!g_pools[i].in_use) {
            p = &g_pools[i];
            break;
        }
    }
    if (!p) {
        cgrtos_exit_critical();
        return 0;
    }

    stack = (uint32_t *)cgrtos_malloc(block_count * sizeof(uint32_t));
    if (!stack) {
        cgrtos_exit_critical();
        return 0;
    }

    p->in_use = 1;
    p->base = (uint8_t *)storage;
    p->block_size = block_size;
    p->block_count = block_count;
    p->free_count = block_count;
    p->free_stack = stack;
    p->free_top = block_count;
    p->magic = MEMPOOL_MAGIC;
    for (i = 0; i < block_count; i++) {
        stack[i] = i;
    }
    cgrtos_exit_critical();
    return (cgrtos_mempool_t *)p;
}

/**
 * @brief 从内存池分配一块
 * @details 从空闲栈弹出索引并返回对应块指针。持临界区 O(1)，不切换任务。
 * @param[in] pool 内存池句柄
 * @return 块指针；池空或句柄无效时返回 NULL
 * @retval 非 NULL 对齐后的块首地址
 * @retval NULL    参数无效或池已满
 * @note 可在任务上下文安全调用；ISR 中须确保无并发 alloc/free
 * @warning 返回指针未清零
 * @attention ✅ ISR；❌ block/switch
 */
void *cgrtos_mempool_alloc(cgrtos_mempool_t *pool)
{
    mempool_ctrl_t *p = (mempool_ctrl_t *)pool;
    uint32_t idx;
    void *ptr;

    if (!p || p->magic != MEMPOOL_MAGIC || !p->in_use) {
        return 0;
    }
    cgrtos_enter_critical();
    if (p->free_top == 0) {
        cgrtos_exit_critical();
        return 0;
    }
    p->free_top--;
    idx = p->free_stack[p->free_top];
    p->free_count--;
    ptr = p->base + (uint64_t)idx * p->block_size;
    cgrtos_exit_critical();
    return ptr;
}

/**
 * @brief 归还块到内存池
 * @details 校验 ptr 属于 pool 且块对齐，将索引压回空闲栈。持临界区 O(1)，不切换任务。
 * @param[in] pool 内存池句柄
 * @param[in] ptr  先前 cgrtos_mempool_alloc 返回的指针
 * @return pdPASS 成功；errPARAM 参数无效；errOVERFLOW 重复释放或栈溢出
 * @retval pdPASS      归还成功
 * @retval errPARAM    句柄/指针/对齐/索引无效
 * @retval errOVERFLOW 双重释放或空闲栈已满
 * @note 不校验块内容
 * @warning 双重释放返回 errOVERFLOW
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_mempool_free(cgrtos_mempool_t *pool, void *ptr)
{
    mempool_ctrl_t *p = (mempool_ctrl_t *)pool;
    uintptr_t off;
    uint32_t idx;

    if (!p || p->magic != MEMPOOL_MAGIC || !p->in_use || !ptr) {
        return errPARAM;
    }
    off = (uintptr_t)((uint8_t *)ptr - p->base);
    if (off % p->block_size != 0) {
        return errPARAM;
    }
    idx = (uint32_t)(off / p->block_size);
    if (idx >= p->block_count) {
        return errPARAM;
    }

    cgrtos_enter_critical();
    if (p->free_top >= p->block_count) {
        cgrtos_exit_critical();
        return errOVERFLOW; /* double free / 池满栈 */
    }
    p->free_stack[p->free_top++] = idx;
    p->free_count++;
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 销毁内存池并释放控制资源
 * @details 释放空闲索引栈（cgrtos_free），清零 magic 并释放槽位。持临界区，不切换任务。
 * @param[in] pool 内存池句柄
 * @return pdPASS 成功；errPARAM 句柄无效
 * @retval pdPASS   销毁成功
 * @retval errPARAM 句柄或 magic 无效
 * @note 不释放用户 storage 区域，仅释放内部 free_stack
 * @warning 销毁后所有已分配块指针失效
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_mempool_delete(cgrtos_mempool_t *pool)
{
    mempool_ctrl_t *p = (mempool_ctrl_t *)pool;
    if (!p || p->magic != MEMPOOL_MAGIC) {
        return errPARAM;
    }
    cgrtos_enter_critical();
    if (p->free_stack) {
        cgrtos_free(p->free_stack);
        p->free_stack = 0;
    }
    p->in_use = 0;
    p->magic = 0;
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 查询内存池剩余空闲块数
 * @details 只读返回 free_count，不持锁（单字段读）。不阻塞、不切换。
 * @param[in] pool 内存池句柄
 * @return 空闲块数；句柄无效返回 0
 * @retval >=0 当前空闲块计数
 * @note 并发 alloc/free 时读数可能略有延迟
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
uint32_t cgrtos_mempool_free_count(cgrtos_mempool_t *pool)
{
    mempool_ctrl_t *p = (mempool_ctrl_t *)pool;
    if (!p || p->magic != MEMPOOL_MAGIC) {
        return 0;
    }
    return p->free_count;
}
