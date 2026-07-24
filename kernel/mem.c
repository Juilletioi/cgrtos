/**
 * @file mem.c
 * @brief TLSF 堆分配器实现
 * @details Two-Level Segregated Fit（TLSF）堆，malloc/free 摊还 O(1)。
 *          参考：Masmano et al., EUROMICRO 2004。
 *
 *          块头位于用户指针之前；size 低比特标记 FREE；
 *          合并时需更新物理后继的 prev_phys（正确性关键）。
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 */
#include "cgrtos.h"
#include <string.h>

/** @def TLSF_ALIGN 堆块对齐字节数 */
#define TLSF_ALIGN          8U
/** @def TLSF_ALIGN_LOG2 log2(TLSF_ALIGN) */
#define TLSF_ALIGN_LOG2     3U
/** @def TLSF_ALIGN_MASK 对齐掩码 */
#define TLSF_ALIGN_MASK     (TLSF_ALIGN - 1U)
/** @def TLSF_MIN_BLOCK 最小可分配块净荷（字节） */
#define TLSF_MIN_BLOCK      32U
/** @def TLSF_FL_BITS 一级索引位数 */
#define TLSF_FL_BITS        5U
/** @def TLSF_SL_BITS 二级索引位数 */
#define TLSF_SL_BITS        5U
/** @def TLSF_FL_COUNT 一级桶数量 */
#define TLSF_FL_COUNT       (1U << TLSF_FL_BITS)
/** @def TLSF_SL_COUNT 二级桶数量 */
#define TLSF_SL_COUNT       (1U << TLSF_SL_BITS)
/** @def TLSF_FL_SHIFT 一级/二级分界：SMALL = 1<<FL_SHIFT（须为 SL_BITS+ALIGN_LOG2） */
#define TLSF_FL_SHIFT       (TLSF_SL_BITS + TLSF_ALIGN_LOG2)
/** @def TLSF_FL_MAX 最大一级索引 */
#define TLSF_FL_MAX         (TLSF_FL_COUNT - 1U)
/** @def TLSF_SL_MAX 最大二级索引 */
#define TLSF_SL_MAX         (TLSF_SL_COUNT - 1U)
/** @def TLSF_BLOCK_HDR 块头结构体大小 */
#define TLSF_BLOCK_HDR      sizeof(tlsf_block_t)
/** @def TLSF_BLOCK_FREE size 字段空闲位 */
#define TLSF_BLOCK_FREE     1U
/** @def TLSF_BLOCK_USED size 字段已用（无空闲位） */
#define TLSF_BLOCK_USED     0U
/** @def TLSF_BLOCK_MAGIC 块魔数，用于校验 */
#define TLSF_BLOCK_MAGIC    0xC0DECAFEU
/** @def TLSF_REDZONE_MAGIC 用户区尾金丝雀 */
#define TLSF_REDZONE_MAGIC  0xBEEFCAFEu
/** @def TLSF_POISON_BYTE 释放后喷毒字节 */
#define TLSF_POISON_BYTE    0xA5u

/** @brief TLSF 内存块头 */
typedef struct tlsf_block {
    uint32_t            size;      /**< 净荷大小 + FREE 标志 */
    uint32_t            magic;     /**< 魔数 TLSF_BLOCK_MAGIC */
    struct tlsf_block  *prev_phys; /**< 物理前驱块 */
    struct tlsf_block  *next_free; /**< 空闲链表后继 */
    struct tlsf_block  *prev_free; /**< 空闲链表前驱 */
} tlsf_block_t;

/** @brief TLSF 空闲链表池控制结构 */
typedef struct {
    uint32_t            fl_bitmap;                          /**< 一级非空位图 */
    uint32_t            sl_bitmap[TLSF_FL_COUNT];           /**< 各一级二级位图 */
    tlsf_block_t       *block_list[TLSF_FL_COUNT][TLSF_SL_COUNT]; /**< 空闲链表头 */
} tlsf_pool_t;

/** @brief 静态堆缓冲区 */
static uint8_t          g_heap[CONFIG_HEAP_SIZE];
/** @brief TLSF 池状态 */
static tlsf_pool_t      g_pool;
/** @brief 当前已用堆字节数 */
static uint32_t         g_heap_used;
/** @brief 历史最小剩余空闲字节 */
static uint32_t         g_heap_min_free;
/** @brief 堆是否已初始化 */
static int              g_heap_init;

#if CONFIG_USE_HOOKS
extern cgrtos_malloc_failed_hook_t g_malloc_failed_hook;
#endif

/**
 * @brief 将字节数向上对齐到 TLSF_ALIGN（8 字节）
 * @details 加上 TLSF_ALIGN_MASK 后按位与 ~MASK，实现向上取整到 8 字节边界。
 * @param[in] size 原始请求大小
 * @return 对齐后的字节数
 * @retval >=size 8 字节对齐结果
 * @note 内联热路径，无锁
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static inline uint32_t tlsf_align_up(uint32_t size)
{
    return (size + TLSF_ALIGN_MASK) & ~TLSF_ALIGN_MASK;
}

/**
 * @brief 从块头 size 字段提取净荷大小（清除标志位）
 * @details 对 block->size 按位与 ~3U，去掉低 2 比特 FREE/USED 标志。
 * @param[in] block 块头指针
 * @return 净荷字节数（不含块头与标志位）
 * @retval >=0 用户区净荷大小
 * @note 内联热路径
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static inline uint32_t tlsf_block_size(tlsf_block_t *block)
{
    return block->size & ~3U;
}

/**
 * @brief 判断内存块当前是否处于空闲状态
 * @details 检查 block->size 的 TLSF_BLOCK_FREE 标志位是否置位。
 * @param[in] block 块头指针
 * @return 非零表示空闲；零表示已分配
 * @retval 1 空闲块
 * @retval 0 已分配块
 * @note 内联热路径
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static inline int tlsf_block_is_free(tlsf_block_t *block)
{
    return (block->size & TLSF_BLOCK_FREE) != 0;
}

/**
 * @brief 设置块的净荷大小与空闲/已用标志
 * @details 将 size 与 free 标志 OR 后写入 block->size。
 * @param[in] block 块头指针
 * @param[in] size  净荷字节数（不含标志位）
 * @param[in] free  非零则标记 FREE，否则 USED
 * @return 无
 * @retval 无
 * @note 内联热路径
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static inline void tlsf_set_block_size(tlsf_block_t *block, uint32_t size, int free)
{
    block->size = size | (free ? TLSF_BLOCK_FREE : 0U);
}

/**
 * @brief 按物理布局计算当前块的后继块头指针
 * @details 后继地址 = block + TLSF_BLOCK_HDR + tlsf_block_size(block)。
 * @param[in] block 当前块头
 * @return 紧邻其后的下一块块头（物理地址连续）
 * @retval 非 NULL 物理后继块头
 * @note 调用方须校验地址仍在堆范围内
 * @warning 越界后继指针将导致合并损坏堆
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static inline tlsf_block_t *tlsf_block_next(tlsf_block_t *block)
{
    return (tlsf_block_t *)((uint8_t *)block + TLSF_BLOCK_HDR + tlsf_block_size(block));
}

/**
 * @brief 由用户可见指针反推块头地址
 * @details 用户指针减去 TLSF_BLOCK_HDR 偏移即得块头。
 * @param[in] ptr cgrtos_malloc 返回的用户指针
 * @return 位于 ptr 之前的 tlsf_block_t 块头指针
 * @retval 非 NULL 对应块头
 * @note 仅对有效堆指针有意义
 * @warning 非法 ptr 将导致堆元数据损坏
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static inline tlsf_block_t *tlsf_block_from_ptr(void *ptr)
{
    return (tlsf_block_t *)((uint8_t *)ptr - TLSF_BLOCK_HDR);
}

/**
 * @brief 将请求净荷大小映射到 TLSF 一级(fl)/二级(sl)桶索引
 * @details 小于 TLSF_MIN_BLOCK 的请求提升到 MIN_BLOCK；小块区 fl=0 线性分桶；
 *          大块区按 Matt Conte TLSF 公式计算 fl_idx 与 sl_idx 并钳制上界。
 * @param[in]  size 请求净荷大小（字节）
 * @param[out] fl   一级索引
 * @param[out] sl   二级索引
 * @return 无
 * @retval 无
 * @note 输出 fl/sl 用于位图查找与链表操作
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static void tlsf_mapping(uint32_t size, uint32_t *fl, uint32_t *sl)
{
    /* 1. 小于 TLSF_MIN_BLOCK 的请求提升到 MIN_BLOCK */
    if (size < TLSF_MIN_BLOCK) {
        size = TLSF_MIN_BLOCK;
    }

    if (size < (1U << TLSF_FL_SHIFT)) {
        /* 2. 小块区：fl=0，sl=size>>ALIGN_LOG2（线性分桶） */
        *fl = 0;
        *sl = size >> TLSF_ALIGN_LOG2;
        if (*sl >= TLSF_SL_COUNT) {
            *sl = TLSF_SL_COUNT - 1U;
        }
    } else {
        /* 3. 大块区：对数一级 + 二级细分 */
        uint32_t fls = 31U - (uint32_t)__builtin_clz(size);
        uint32_t sl_idx = (size >> (fls - TLSF_SL_BITS)) ^ TLSF_SL_COUNT;
        uint32_t fl_idx = fls - (TLSF_FL_SHIFT - 1U);
        if (fl_idx >= TLSF_FL_COUNT) {
            fl_idx = TLSF_FL_COUNT - 1U;
        }
        if (sl_idx >= TLSF_SL_COUNT) {
            sl_idx = TLSF_SL_COUNT - 1U;
        }
        *fl = fl_idx;
        *sl = sl_idx;
    }
}

/**
 * @brief 从指定 (fl,sl) 空闲链表中移除块
 * @details 更新 prev_free/next_free 与 block_list；链表变空时清除 sl_bitmap 与 fl_bitmap 位。
 * @param[in] pool  TLSF 池控制结构
 * @param[in] block 待移除块（须在链表中）
 * @param[in] fl    一级桶索引
 * @param[in] sl    二级桶索引
 * @return 无
 * @retval 无
 * @note 调用方须已持临界区
 * @warning block 不在该桶时将损坏空闲链
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static void tlsf_list_remove(tlsf_pool_t *pool, tlsf_block_t *block,
                             uint32_t fl, uint32_t sl)
{
    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        pool->block_list[fl][sl] = block->next_free;
        if (!pool->block_list[fl][sl]) {
            pool->sl_bitmap[fl] &= ~(1U << sl);
            if (!pool->sl_bitmap[fl]) {
                pool->fl_bitmap &= ~(1U << fl);
            }
        }
    }
    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }
    block->next_free = 0;
    block->prev_free = 0;
}

/**
 * @brief 将空闲块头插入指定 (fl,sl) 链表
 * @details 头插法挂入 block_list，并置位 sl_bitmap[fl] 与 fl_bitmap。
 * @param[in] pool  TLSF 池控制结构
 * @param[in] block 待插入空闲块
 * @param[in] fl    一级桶索引
 * @param[in] sl    二级桶索引
 * @return 无
 * @retval 无
 * @note 调用方须已持临界区
 * @warning 重复插入同一 block 将形成环
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static void tlsf_list_insert(tlsf_pool_t *pool, tlsf_block_t *block,
                             uint32_t fl, uint32_t sl)
{
    block->next_free = pool->block_list[fl][sl];
    block->prev_free = 0;
    if (block->next_free) {
        block->next_free->prev_free = block;
    }
    pool->block_list[fl][sl] = block;
    pool->sl_bitmap[fl] |= (1U << sl);
    pool->fl_bitmap |= (1U << fl);
}

/**
 * @brief 在位图辅助下查找不小于 size 的首个合适空闲块
 * @details tlsf_mapping 得起始 (fl,sl)；在当前 fl 内用 sl_bitmap 掩码找桶，
 *          遍历空闲链返回首个 tlsf_block_size >= size 的块；否则搜索更高桶。
 * @param[in] size 请求净荷大小（字节）
 * @return 满足 size 的空闲块指针；堆无足够空间返回 NULL
 * @retval 非 NULL 可用空闲块
 * @retval NULL    堆空间不足
 * @note 调用方须已持临界区
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static tlsf_block_t *tlsf_find_suitable(uint32_t size)
{
    uint32_t fl, sl;
    tlsf_mapping(size, &fl, &sl);

    uint32_t sl_map = g_pool.sl_bitmap[fl] & (~0U << sl);
    if (!sl_map) {
        uint32_t fl_map = (fl >= 31U) ? 0U :
                          (g_pool.fl_bitmap & (~0U << (fl + 1U)));
        if (!fl_map) {
            return 0;
        }
        fl = (uint32_t)__builtin_ctz(fl_map);
        sl_map = g_pool.sl_bitmap[fl];
    }
    sl = (uint32_t)__builtin_ctz(sl_map);

    /* 遍历当前桶，mapping 为 first-fit — 仍须验证实际 size */
    for (tlsf_block_t *block = g_pool.block_list[fl][sl]; block;
         block = block->next_free) {
        if (tlsf_block_size(block) >= size) {
            return block;
        }
    }

    /* 当前桶耗尽：搜索更高索引桶 */
    sl_map &= ~((1U << (sl + 1U)) - 1U);
    while (1) {
        if (!sl_map) {
            uint32_t fl_map = (fl >= 31U) ? 0U :
                              (g_pool.fl_bitmap & (~0U << (fl + 1U)));
            if (!fl_map) {
                return 0;
            }
            fl = (uint32_t)__builtin_ctz(fl_map);
            sl_map = g_pool.sl_bitmap[fl];
        }
        sl = (uint32_t)__builtin_ctz(sl_map);
        for (tlsf_block_t *block = g_pool.block_list[fl][sl]; block;
             block = block->next_free) {
            if (tlsf_block_size(block) >= size) {
                return block;
            }
        }
        sl_map &= ~(1U << sl);
    }
}

/**
 * @brief 将过大的空闲块拆分为「已分配块 + 剩余空闲块」
 * @details 余量不足 MIN_BLOCK 时不拆分；否则创建 remain 块、缩小 block、
 *          更新物理后继 prev_phys 并将 remain 入空闲链。
 * @param[in] block 待拆分空闲块（仍标记 FREE）
 * @param[in] size  目标分配净荷大小
 * @return 无
 * @retval 无
 * @note 调用方须已持临界区
 * @warning 对过小余块强行拆分将破坏堆结构
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static void tlsf_split_block(tlsf_block_t *block, uint32_t size)
{
    uint32_t total = tlsf_block_size(block);
    /* 1. 若 total < size + HDR + MIN_BLOCK，剩余太小无法拆分，整块保留 */
    if (total < size + TLSF_BLOCK_HDR + TLSF_MIN_BLOCK) {
        return;
    }

    /* 2. 在 block 净荷末尾创建 remain 块，设置 size 与 prev_phys */
    tlsf_block_t *remain = (tlsf_block_t *)((uint8_t *)block + TLSF_BLOCK_HDR + size);
    tlsf_set_block_size(remain, total - size - TLSF_BLOCK_HDR, 1);
    remain->magic = TLSF_BLOCK_MAGIC;
    remain->prev_phys = block;
    tlsf_set_block_size(block, size, 1);

    /* 3. 缩小 block 净荷；更新物理后继 next->prev_phys 指向 remain */
    tlsf_block_t *next = tlsf_block_next(remain);
    if ((uint8_t *)next < g_heap + CONFIG_HEAP_SIZE) {
        next->prev_phys = remain;
    }

    /* 4. 对 remain 做 tlsf_mapping 并 tlsf_list_insert 入空闲链 */
    uint32_t fl, sl;
    tlsf_mapping(tlsf_block_size(remain), &fl, &sl);
    tlsf_list_insert(&g_pool, remain, fl, sl);
}

/**
 * @brief 将空闲块与物理相邻空闲块合并后重新入链
 * @details 先后继、再前驱合并；每次合并更新物理后继 prev_phys；
 *          最终将合并块 tlsf_mapping 并 tlsf_list_insert 挂回空闲链。
 * @param[in] block 待合并块（须已标记为 FREE，且尚未在链表中）
 * @return 无
 * @retval 无
 * @note 调用方须已持临界区
 * @warning 遗漏 prev_phys 更新将导致后续合并/释放错误
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static void tlsf_merge_block(tlsf_block_t *block)
{
    /* 1. 若物理后继 next 空闲：从链中移除 next，扩展 block 净荷 */
    tlsf_block_t *next = tlsf_block_next(block);
    if ((uint8_t *)next < g_heap + CONFIG_HEAP_SIZE && tlsf_block_is_free(next)) {
        uint32_t fl, sl;
        tlsf_mapping(tlsf_block_size(next), &fl, &sl);
        tlsf_list_remove(&g_pool, next, fl, sl);
        tlsf_set_block_size(block, tlsf_block_size(block) + TLSF_BLOCK_HDR +
                            tlsf_block_size(next), 1);
        tlsf_block_t *after = tlsf_block_next(block);
        if ((uint8_t *)after < g_heap + CONFIG_HEAP_SIZE) {
            after->prev_phys = block;
        }
    }

    /* 2. 若物理前驱 prev 空闲：从链中移除 prev，合并到 prev */
    if (block->prev_phys && tlsf_block_is_free(block->prev_phys)) {
        tlsf_block_t *prev = block->prev_phys;
        uint32_t fl, sl;
        tlsf_mapping(tlsf_block_size(prev), &fl, &sl);
        tlsf_list_remove(&g_pool, prev, fl, sl);
        tlsf_set_block_size(prev, tlsf_block_size(prev) + TLSF_BLOCK_HDR +
                            tlsf_block_size(block), 1);
        block = prev;
        /* 3. 合并后再次更新物理后继的 prev_phys */
        tlsf_block_t *after = tlsf_block_next(block);
        if ((uint8_t *)after < g_heap + CONFIG_HEAP_SIZE) {
            after->prev_phys = block;
        }
    }

    /* 4. tlsf_mapping + tlsf_list_insert 将最终块挂回空闲链 */
    uint32_t fl, sl;
    tlsf_mapping(tlsf_block_size(block), &fl, &sl);
    tlsf_list_insert(&g_pool, block, fl, sl);
}

/**
 * @brief 更新历史最小剩余堆字节数 g_heap_min_free
 * @details 计算 free_bytes = CONFIG_HEAP_SIZE - g_heap_used，若小于 g_heap_min_free 则更新记录。
 * @return 无
 * @retval 无
 * @note 供 cgrtos_get_min_free_heap 查询
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static void heap_update_min_free(void)
{
    uint32_t free_bytes = CONFIG_HEAP_SIZE - g_heap_used;
    if (free_bytes < g_heap_min_free) {
        g_heap_min_free = free_bytes;
    }
}

/**
 * @brief 初始化 TLSF 池，将整堆作为单块空闲区挂入桶 0
 * @details 清零 g_pool 位图与链表头；在 g_heap 起始处放置首块并标记 FREE；
 *          tlsf_mapping + list_insert 入空闲链；设置 g_heap_used=0、g_heap_min_free=HEAP_SIZE、g_heap_init=1。
 * @return 无
 * @retval 无
 * @note 由 cgrtos_malloc 首次调用惰性触发
 * @warning 重复初始化将损坏已有分配
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static void tlsf_init_pool(void)
{
    memset(&g_pool, 0, sizeof(g_pool));

    tlsf_block_t *block = (tlsf_block_t *)g_heap;
    uint32_t total = CONFIG_HEAP_SIZE - TLSF_BLOCK_HDR;
    tlsf_set_block_size(block, total, 1);
    block->magic = TLSF_BLOCK_MAGIC;
    block->prev_phys = 0;
    block->next_free = 0;
    block->prev_free = 0;

    uint32_t fl, sl;
    tlsf_mapping(total, &fl, &sl);
    tlsf_list_insert(&g_pool, block, fl, sl);

    g_heap_used = 0;
    g_heap_min_free = CONFIG_HEAP_SIZE;
    g_heap_init = 1;
}

/**
 * @brief 分配堆内存（TLSF O(1) 摊还）
 * @details 首次调用惰性 tlsf_init_pool；对齐并提升 size 至 MIN_BLOCK；
 *          临界区内 find/split/标记 USED，更新 g_heap_used 与 min_free，返回用户指针。
 * @param[in] size 请求字节数
 * @return 8 字节对齐的用户指针；失败返回 NULL
 * @retval 非 NULL 有效堆块
 * @retval NULL    size==0 或堆空间不足
 * @note 线程安全依赖 cgrtos_enter_critical / exit_critical
 * @warning 返回指针未清零（CONFIG_HEAP_POISON 时填充 0xCD）
 * @attention ❌ ISR；❌ block/switch
 */
void *cgrtos_malloc(unsigned long size)
{
    /* 1. 首次调用惰性 tlsf_init_pool；size==0 返回 NULL */
    if (!g_heap_init) {
        tlsf_init_pool();
    }
    if (size == 0) {
        return 0;
    }

    uint32_t req = tlsf_align_up((uint32_t)size);
    if (req < TLSF_MIN_BLOCK) {
        req = TLSF_MIN_BLOCK;
    }

    /* 2. 将 size 对齐并提升到 MIN_BLOCK，进入临界区 */
    cgrtos_enter_critical();

    /* 3. tlsf_find_suitable 找块；失败则可选触发 malloc_failed_hook */
    tlsf_block_t *block = tlsf_find_suitable(req);
    if (!block) {
        cgrtos_exit_critical();
#if CONFIG_USE_HOOKS
        if (g_malloc_failed_hook) {
            g_malloc_failed_hook(size);
        }
#endif
        return 0;
    }

    uint32_t fl, sl;
    tlsf_mapping(tlsf_block_size(block), &fl, &sl);
    tlsf_list_remove(&g_pool, block, fl, sl);

    /* 4. 从空闲链移除，tlsf_split_block 拆分余量，标记 USED，更新 g_heap_used */
    tlsf_split_block(block, req);
    tlsf_set_block_size(block, tlsf_block_size(block), 0);
    block->magic = TLSF_BLOCK_MAGIC;

    g_heap_used += TLSF_BLOCK_HDR + tlsf_block_size(block);
    /* 5. heap_update_min_free，返回 block+HDR 的用户指针 */
    heap_update_min_free();

    void *ptr = (void *)((uint8_t *)block + TLSF_BLOCK_HDR);
#if CONFIG_HEAP_POISON
    memset(ptr, 0xCD, req);
#endif
#if CONFIG_HEAP_REDZONE
    /* 在用户请求 size 之后写 4 字节金丝雀（需块够大；req 已对齐） */
    if (tlsf_block_size(block) >= req + 4U) {
        uint32_t *rz = (uint32_t *)((uint8_t *)ptr + req);
        *rz = TLSF_REDZONE_MAGIC;
        /* 把实际请求长度藏在 magic 旁：用 prev_free 暂存 req（USED 块不用 free 链） */
        block->prev_free = (tlsf_block_t *)(uintptr_t)req;
    } else {
        block->prev_free = 0;
    }
#endif
    cgrtos_exit_critical();
    return ptr;
}

/**
 * @brief 分配 count*size 字节并清零
 * @details 检测 count*size 乘法溢出；成功则 cgrtos_malloc 后 memset 清零。
 * @param[in] count 元素个数
 * @param[in] size  每个元素字节数
 * @return 用户指针；溢出或 malloc 失败返回 NULL
 * @retval 非 NULL 已清零堆块
 * @retval NULL    溢出或分配失败
 * @note 等价于标准 calloc 语义
 * @warning 大 count*size 可能触发 malloc_failed_hook
 * @attention ❌ ISR；❌ block/switch
 */
void *cgrtos_calloc(unsigned long count, unsigned long size)
{
    if (count != 0 && size > (~(unsigned long)0UL) / count) {
        return 0;
    }
    unsigned long total = count * size;
    void *ptr = cgrtos_malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

/**
 * @brief 释放先前 cgrtos_malloc/calloc 返回的堆内存
 * @details ptr 为空或堆未初始化则返回；临界区内反推 block、校验 magic 与 FREE 标志；
 *          递减 g_heap_used、标记 FREE、tlsf_merge_block 合并邻居并更新 min_free。
 * @param[in] ptr 用户指针；NULL 安全忽略
 * @return 无
 * @retval 无
 * @note double-free 或 magic 错误时记录日志并忽略
 * @warning CONFIG_HEAP_REDZONE 开启时越界写会在 free 时检测
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_free(void *ptr)
{
    /* 1. ptr 为空或堆未初始化则返回 */
    if (!ptr || !g_heap_init) {
        return;
    }

    cgrtos_enter_critical();

    /* 2. 由 ptr 反推 block，校验 magic 且非 FREE（防 double-free） */
    tlsf_block_t *block = tlsf_block_from_ptr(ptr);
    if (block->magic != TLSF_BLOCK_MAGIC || tlsf_block_is_free(block)) {
        cgrtos_exit_critical();
        CGRTOS_LOGE("heap", "double-free or bad magic");
        return;
    }

#if CONFIG_HEAP_REDZONE
    if (block->prev_free) {
        uint32_t req = (uint32_t)(uintptr_t)block->prev_free;
        if (tlsf_block_size(block) >= req + 4U) {
            uint32_t *rz = (uint32_t *)((uint8_t *)ptr + req);
            if (*rz != TLSF_REDZONE_MAGIC) {
                cgrtos_exit_critical();
                CGRTOS_LOGE("heap", "buffer overrun (redzone)");
                return;
            }
        }
    }
#endif

    /* 3. 递减 g_heap_used，标记 FREE，tlsf_merge_block 与邻居合并 */
    g_heap_used -= TLSF_BLOCK_HDR + tlsf_block_size(block);
#if CONFIG_HEAP_POISON
    memset(ptr, TLSF_POISON_BYTE, tlsf_block_size(block));
#endif
    tlsf_set_block_size(block, tlsf_block_size(block), 1);
    block->prev_free = 0;
    tlsf_merge_block(block);
    /* 4. heap_update_min_free，退出临界区 */
    heap_update_min_free();

    cgrtos_exit_critical();
}

/**
 * @brief 查询当前剩余可用堆字节数
 * @details 返回 CONFIG_HEAP_SIZE - g_heap_used；只读快照，不持锁。
 * @return 当前空闲堆字节数
 * @retval >=0 剩余可用字节
 * @note 并发 alloc/free 时读数可能略有延迟
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
unsigned long cgrtos_get_free_heap(void)
{
    return CONFIG_HEAP_SIZE - g_heap_used;
}

/**
 * @brief 查询运行以来出现过的最小剩余堆字节数（高水位反向指标）
 * @details 返回 g_heap_min_free，由 heap_update_min_free 在每次 alloc/free 时维护。
 * @return 历史最小空闲堆字节数
 * @retval >=0 观测到的最小剩余量
 * @note 可用于诊断内存压力与泄漏趋势
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
unsigned long cgrtos_get_min_free_heap(void)
{
    return g_heap_min_free;
}
