/**
 * @file list.c
 * @brief 内核双向链表实现
 * @details 侵入式双向链表的初始化、尾部插入、有序插入与删除。
 *          供调度器 CFS/EDF 就绪队列、延迟唤醒链表及 EDF 释放轮使用。
 */
#include "list.h"

/**
 * @brief 初始化空链表。
 *
 * @param list 链表头指针。
 *
 * @details
 * 1. 将 head 与 tail 置 NULL，表示空链。
 * 2. 将 count 置 0。
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
 * @brief 初始化链表节点。
 *
 * @param item 待初始化节点。
 *
 * @details
 * 1. 将 next 与 prev 置 NULL，断开与任何链表的关联。
 * 2. 将 value 排序键置 0。
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
 * @brief 在链表尾部插入节点。
 *
 * @param list 目标链表。
 * @param item 待插入节点。
 *
 * @details
 * 1. 将 item->next 置 NULL（成为新尾）。
 * 2. 将 item->prev 指向当前 tail。
 * 3. 若 tail 非空，更新 tail->next 指向 item；否则 head 也指向 item。
 * 4. 更新 list->tail 为 item，count 递增。
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
 * @brief 按 value 升序插入（允许相等值排在后面）。
 *
 * @param list  目标链表。
 * @param item  待插入节点。
 * @param value 排序键。
 *
 * @details
 * 1. 将 item->value 设为排序键 value。
 * 2. 从头遍历，跳过 value <= 插入键的节点（相等值排在后面）。
 * 3. 若遍历到末尾，调用 list_insert_end 追加。
 * 4. 否则在 cur 前插入：更新 item 与 cur 及 cur->prev 的 prev/next 指针。
 * 5. count 递增。
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
 * @brief 按 value 严格升序插入（相等值排在后面）。
 *
 * @param list  目标链表。
 * @param item  待插入节点。
 * @param value 排序键。
 *
 * @details
 * 1. 将 item->value 设为排序键 value。
 * 2. 从头遍历，跳过 value < 插入键的节点（相等时不跳过，保证稳定排序）。
 * 3. 若遍历到末尾，调用 list_insert_end 追加。
 * 4. 否则在 cur 前插入并更新前后指针。
 * 5. count 递增。
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
 * @brief 从链表中移除节点。
 *
 * @param list 目标链表。
 * @param item 待移除节点。
 *
 * @details
 * 1. 若 item 有前驱，更新前驱的 next；否则 item 是 head，更新 list->head。
 * 2. 若 item 有后继，更新后继的 prev；否则 item 是 tail，更新 list->tail。
 * 3. 将 item 的 next/prev 置 NULL，断开链接。
 * 4. 若 count > 0 则递减。
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
 * @brief 查看链表首节点（不移除）。
 *
 * @param list 目标链表。
 * @return 首节点指针，空链表返回 NULL。
 *
 * @details
 * 1. 直接返回 list->head，不修改链表结构。
 */
list_item_t *list_peek_head(list_t *list)
{
    /* 1. 直接返回 head，不修改链表 */
    return list->head;
}

/**
 * @brief 获取链表节点数量。
 *
 * @param list 目标链表。
 * @return 节点计数。
 *
 * @details
 * 1. 返回 list->count 字段（由插入/删除操作维护）。
 */
uint32_t list_count(list_t *list)
{
    /* 1. 返回维护好的 count 字段 */
    return list->count;
}
