/**
 * @file mempool.c
 * @brief 固定块静态内存池（模块3）：无碎片、确定时延
 */
#include "cgrtos.h"
#include <string.h>

#ifndef CGRTOS_MAX_MEMPOOL
#define CGRTOS_MAX_MEMPOOL 8
#endif

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

static mempool_ctrl_t g_pools[CGRTOS_MAX_MEMPOOL];

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

uint32_t cgrtos_mempool_free_count(cgrtos_mempool_t *pool)
{
    mempool_ctrl_t *p = (mempool_ctrl_t *)pool;
    if (!p || p->magic != MEMPOOL_MAGIC) {
        return 0;
    }
    return p->free_count;
}
