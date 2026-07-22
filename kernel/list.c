/**
 * @file list.c
 * @brief 内核侵入式双向链表实现
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * list.h 中 API 的具体实现：初始化、尾部插入、按 value 排序插入及 O(1) 删除。
 * 供调度器就绪队列、延迟唤醒链表等内核子系统复用。
 */

#include "list.h"

/**
 * @brief 初始化空链表
 * @details 将 head/tail 置 NULL、count 置 0。纯指针操作，不获取锁、不触发调度；调用方须保证独占访问。
 * @param[out] list 待初始化的链表头
 * @return 无
 * @retval 无
 * @note 链表节点内存由调用方管理，本函数不分配内存
 * @warning 对已在使用中的链表重复 init 将导致原有节点泄漏
 * @attention ✅ ISR；❌ block/switch
 */
void list_init(list_t *list)
{
    /* 1. head/tail 置 NULL 表示空链 */
    list->head = 0;
    list->tail = 0;
    /* 2. 节点计数清零 */
    list->count = 0;
}

/**
 * @brief 初始化链表节点
 * @details 断开 next/prev 链接并将 value 清零。纯指针操作，可在任意上下文调用（须保证无并发写）。
 * @param[out] item 待初始化的链表节点
 * @return 无
 * @retval 无
 * @note 插入前须确保节点未挂接在其他链表中
 * @warning 对仍在链表中的节点调用将导致链表结构损坏
 * @attention ✅ ISR；❌ block/switch
 */
void list_init_item(list_item_t *item)
{
    /* 1. 断开前后链接 */
    item->next = 0;
    item->prev = 0;
    /* 2. 排序键清零 */
    item->value = 0;
}

/**
 * @brief 在链表尾部插入节点
 * @details 将 item 追加为新的 tail，更新 head（空链时）并递增 count。O(1) 操作，不阻塞、不切换任务。
 * @param[inout] list 目标链表
 * @param[inout] item 待插入节点（须已 list_init_item 且未挂接）
 * @return 无
 * @retval 无
 * @note 供 CFS/EDF 就绪队列尾部入队使用
 * @warning item 必须不在任何链表中，否则产生环
 * @attention ✅ ISR；❌ block/switch
 */
void list_insert_end(list_t *list, list_item_t *item)
{
    /* 1. item 成为新尾节点 */
    item->next = 0;
    /* 2. 前驱指向当前 tail */
    item->prev = list->tail;
    /* 3. 更新旧尾 next 或空链时设置 head */
    if (list->tail) {
        list->tail->next = item;
    } else {
        list->head = item;
    }
    /* 4. 更新 tail 并递增 count */
    list->tail = item;
    list->count++;
}

/**
 * @brief 按 value 升序插入（相等值排在后面）
 * @details 遍历链表找到第一个 value > 插入键的位置并在此前插入；相等时排在已有相等节点之后。O(n) 扫描，不阻塞、不切换。
 * @param[inout] list  目标链表
 * @param[inout] item  待插入节点
 * @param[in]     value 排序键
 * @return 无
 * @retval 无
 * @note 用于延迟唤醒链表等允许稳定相等排序的场景
 * @warning 链表未受保护时不可并发修改
 * @attention ✅ ISR；❌ block/switch
 */
void list_insert_sorted(list_t *list, list_item_t *item, uint64_t value)
{
    /* 1. 写入排序键 */
    item->value = value;
    list_item_t *cur = list->head;

    /* 2. 跳过 value <= 插入键的节点 */
    while (cur && cur->value <= value) {
        cur = cur->next;
    }

    /* 3. 遍历到末尾则尾部追加 */
    if (!cur) {
        list_insert_end(list, item);
        return;
    }

    /* 4. 在 cur 前插入并更新前后指针 */
    item->next = cur;
    item->prev = cur->prev;
    if (cur->prev) {
        cur->prev->next = item;
    } else {
        list->head = item;
    }
    cur->prev = item;
    /* 5. 递增 count */
    list->count++;
}

/**
 * @brief 按 value 严格升序插入（相等值排在后面）
 * @details 跳过 value < 插入键的节点，相等时不跳过以保证稳定排序。O(n) 扫描，不阻塞、不切换。
 * @param[inout] list  目标链表
 * @param[inout] item  待插入节点
 * @param[in]     value 排序键
 * @return 无
 * @retval 无
 * @note 与 list_insert_sorted 的区别在于相等键的比较方向（< vs <=）
 * @warning 链表未受保护时不可并发修改
 * @attention ✅ ISR；❌ block/switch
 */
void list_insert_sorted_asc(list_t *list, list_item_t *item, uint64_t value)
{
    /* 1. 写入排序键 */
    item->value = value;
    list_item_t *cur = list->head;

    /* 2. 跳过 value < 插入键的节点（相等不跳过） */
    while (cur && cur->value < value) {
        cur = cur->next;
    }

    /* 3. 遍历到末尾则尾部追加 */
    if (!cur) {
        list_insert_end(list, item);
        return;
    }

    /* 4. 在 cur 前插入并更新前后指针 */
    item->next = cur;
    item->prev = cur->prev;
    if (cur->prev) {
        cur->prev->next = item;
    } else {
        list->head = item;
    }
    cur->prev = item;
    /* 5. 递增 count */
    list->count++;
}

/**
 * @brief 从链表中移除节点
 * @details 更新 head/tail 及相邻节点的 prev/next，断开 item 并递减 count。O(1) 操作，不阻塞、不切换。
 * @param[inout] list 目标链表
 * @param[inout] item 待移除节点（须属于 list）
 * @return 无
 * @retval 无
 * @note 移除后 item 的 next/prev 被置 NULL
 * @warning 移除不属于该链表的节点将导致链表损坏
 * @attention ✅ ISR；❌ block/switch
 */
void list_remove(list_t *list, list_item_t *item)
{
    /* 1. 更新前驱 next 或 head */
    if (item->prev) {
        item->prev->next = item->next;
    } else if (list->head == item) {
        list->head = item->next;
    }

    /* 2. 更新后继 prev 或 tail */
    if (item->next) {
        item->next->prev = item->prev;
    } else if (list->tail == item) {
        list->tail = item->prev;
    }

    /* 3. 断开 item 自身链接 */
    item->next = 0;
    item->prev = 0;
    /* 4. 递减 count */
    if (list->count > 0) {
        list->count--;
    }
}

/**
 * @brief 查看链表首节点（不移除）
 * @details 直接返回 list->head，不修改链表结构。O(1)，不阻塞、不切换。
 * @param[in] list 目标链表
 * @return 首节点指针；空链表返回 NULL
 * @retval 非 NULL 首节点地址
 * @retval NULL    链表为空
 * @note 等价于 peek，不会递减 count
 * @warning 返回指针在并发删除时可能失效，调用方须持锁
 * @attention ✅ ISR；❌ block/switch
 */
list_item_t *list_peek_head(list_t *list)
{
    /* 1. 直接返回 head，不修改链表 */
    return list->head;
}

/**
 * @brief 获取链表节点数量
 * @details 返回由插入/删除维护的 count 字段。O(1)，不阻塞、不切换。
 * @param[in] list 目标链表
 * @return 当前节点数
 * @retval >=0 节点计数
 * @note count 由 list_insert_* / list_remove 自动维护
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
uint32_t list_count(list_t *list)
{
    /* 1. 返回维护好的 count 字段 */
    return list->count;
}
