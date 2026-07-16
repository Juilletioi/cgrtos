/**
 * @file list.h
 * @brief 内核双向链表 API
 * @details 提供侵入式双向链表的初始化、插入、排序插入与删除操作，
 *          供调度器、延迟队列等子系统复用。
 */
#ifndef CGRTOS_LIST_H
#define CGRTOS_LIST_H

#include <stdint.h>

/** @brief 链表节点，嵌入宿主结构体中 */
typedef struct list_item {
    struct list_item *next; /**< 后继节点 */
    struct list_item *prev; /**< 前驱节点 */
    uint64_t          value; /**< 排序键值（可选） */
} list_item_t;

/** @brief 双向链表头 */
typedef struct list {
    list_item_t *head;  /**< 首节点 */
    list_item_t *tail;  /**< 尾节点 */
    uint32_t     count; /**< 当前节点数 */
} list_t;

/**
 * @brief 初始化空链表
 * @param list 链表头指针
 */
void list_init(list_t *list);

/**
 * @brief 初始化链表节点
 * @param item 待初始化节点
 */
void list_init_item(list_item_t *item);

/**
 * @brief 在链表尾部插入节点
 * @param list 目标链表
 * @param item 待插入节点（须已 list_init_item）
 */
void list_insert_end(list_t *list, list_item_t *item);

/**
 * @brief 按 value 升序插入（允许相等值排在后面）
 * @param list  目标链表
 * @param item  待插入节点
 * @param value 排序键
 */
void list_insert_sorted(list_t *list, list_item_t *item, uint64_t value);

/**
 * @brief 按 value 严格升序插入（相等值排在后面）
 * @param list  目标链表
 * @param item  待插入节点
 * @param value 排序键
 */
void list_insert_sorted_asc(list_t *list, list_item_t *item, uint64_t value);

/**
 * @brief 从链表中移除节点
 * @param list 目标链表
 * @param item 待移除节点
 */
void list_remove(list_t *list, list_item_t *item);

/**
 * @brief 查看链表首节点（不移除）
 * @param list 目标链表
 * @return 首节点指针，空链表返回 NULL
 */
list_item_t *list_peek_head(list_t *list);

/**
 * @brief 获取链表节点数量
 * @param list 目标链表
 * @return 节点计数
 */
uint32_t list_count(list_t *list);

#endif /* CGRTOS_LIST_H */
