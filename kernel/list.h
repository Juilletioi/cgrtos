/**
 * @file list.h
 * @brief 内核侵入式双向链表 API
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * 提供侵入式双向链表的初始化、插入、排序插入与删除操作，
 * 供调度器、延迟队列等内核子系统复用。节点嵌入宿主结构体，
 * 通过 list_item_t 成员链接。
 */

#ifndef CGRTOS_LIST_H
#define CGRTOS_LIST_H

#include <stdint.h>

/**
 * @brief 侵入式链表节点
 * @details 嵌入宿主结构体；用户不得直接修改 next/prev 指针，
 *          须通过 list_init_item 及插入/删除 API 维护链接关系。
 */
typedef struct list_item {
    struct list_item *next; ///< 后继节点指针
    struct list_item *prev; ///< 前驱节点指针
    uint64_t          value; ///< 排序键值（list_insert_sorted* 使用；可选）
} list_item_t;

/**
 * @brief 双向链表头
 * @details 内核内部数据结构；用户不得直接读写 head/tail/count，
 *          须通过本头文件 API 操作。
 */
typedef struct list {
    list_item_t *head;  ///< 首节点指针；空链表为 NULL
    list_item_t *tail;  ///< 尾节点指针；空链表为 NULL
    uint32_t     count; ///< 当前节点数量
} list_t;

/**
 * @brief 初始化空链表
 * @details 将 head、tail 置 NULL，count 置 0。纯指针操作，不获取锁、不触发调度。
 * @param[out] list 待初始化的链表头指针，不可为 NULL
 * @return 无
 * @retval 无
 * @note 链表节点内存由调用方管理
 * @warning 对已在使用中的链表重复 init 将导致节点泄漏
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
void list_init(list_t *list);

/**
 * @brief 初始化链表节点
 * @details 将 next、prev 置 NULL，value 置 0；插入前必须调用。
 * @param[out] item 待初始化节点指针，不可为 NULL
 * @return 无
 * @retval 无
 * @note 插入前须确保未挂接在其他链表中
 * @warning 对仍在链表中的节点调用将导致结构损坏
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
void list_init_item(list_item_t *item);

/**
 * @brief 在链表尾部插入节点
 * @details 将 item 链接到 tail 之后；O(1) 操作，不阻塞、不切换。
 * @param[in,out] list 目标链表头指针
 * @param[in,out] item 待插入节点（须已 init 且未挂链）
 * @return 无
 * @retval 无
 * @note 供 CFS/EDF 就绪队列尾部入队
 * @warning 同一 item 不得重复插入
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
void list_insert_end(list_t *list, list_item_t *item);

/**
 * @brief 按 value 升序插入（相等值排在后面）
 * @details 线性扫描插入；相等 value 保持 FIFO 顺序。O(n)，不阻塞、不切换。
 * @param[in,out] list 目标链表
 * @param[in,out] item 待插入节点
 * @param[in] value 排序键，写入 item->value
 * @return 无
 * @retval 无
 * @note 延迟唤醒链表等场景
 * @warning 链表未受保护时不可并发修改
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
void list_insert_sorted(list_t *list, list_item_t *item, uint64_t value);

/**
 * @brief 按 value 严格升序插入（相等值排在后面）
 * @details 跳过 value<插入键；相等不跳过以保证稳定排序。O(n)。
 * @param[in,out] list 目标链表
 * @param[in,out] item 待插入节点
 * @param[in] value 排序键
 * @return 无
 * @retval 无
 * @note CFS vruntime 队列使用
 * @warning 并发修改须持锁
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
void list_insert_sorted_asc(list_t *list, list_item_t *item, uint64_t value);

/**
 * @brief 从链表中移除节点
 * @details 更新 head/tail 及相邻 prev/next，断开 item 并递减 count。O(1)。
 * @param[in,out] list 目标链表
 * @param[in,out] item 待移除节点（须属于 list）
 * @return 无
 * @retval 无
 * @note 移除后 item 的 next/prev 被置 NULL
 * @warning 移除不属于该链表的节点将导致损坏
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
void list_remove(list_t *list, list_item_t *item);

/**
 * @brief 查看链表首节点（不移除）
 * @details 直接返回 list->head，不修改链表结构。O(1)。
 * @param[in] list 目标链表头指针
 * @return 首节点指针；空链表 NULL
 * @retval 非 NULL 首节点
 * @retval NULL 空链
 * @note 等价 peek，不递减 count
 * @warning 并发删除时指针可能失效
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
list_item_t *list_peek_head(list_t *list);

/**
 * @brief 获取链表节点数量
 * @details 返回 list->count。O(1)，不阻塞、不切换。
 * @param[in] list 目标链表头指针
 * @return 当前节点数
 * @retval >=0 节点计数
 * @note 由 insert/remove 自动维护
 * @warning 无
 * @attention ✅ 允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
uint32_t list_count(list_t *list);

#endif /* CGRTOS_LIST_H */
