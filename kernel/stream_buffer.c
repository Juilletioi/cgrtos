/**
 * @file stream_buffer.c
 * @brief StreamBuffer（字节流）与 MessageBuffer（长度前缀整消息）模块
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 本模块提供两类 IPC 缓冲：
 *
 * - **StreamBuffer**：环形字节缓冲，支持部分读写；接收端通过 `trigger` 控制
 *   唤醒水位（可用字节数达到 trigger 才唤醒等待接收的任务）。
 * - **MessageBuffer**：在 StreamBuffer 之上封装，每条消息格式为
 *   `[u32 LE len][payload...]`，send/recv 必须整包操作。
 *
 * 全局静态池 `g_sbs[]` 管理实例；阻塞时挂 send_wait_q/recv_wait_q；
 * 挂入 QueueSet 时发送成功后调用 cgrtos_queue_set_poke。
 *
 * @see cgrtos_stream
 */
#include "cgrtos.h"
#include <string.h>

static cgrtos_stream_buffer_t g_sbs[CGRTOS_MAX_STREAM_BUFFER];

/**
 * @brief 将当前任务挂到指定等待队列并标记阻塞原因
 * @details 读取 running 任务；idle 不可阻塞；调用 sched_block 并 wait_list_add；须在 exit_critical 后 yield。
 * @param[in]     sb      关联的 StreamBuffer 实例
 * @param[in,out] wq      目标等待队列指针（send 或 recv）
 * @param[in]     reason  阻塞原因枚举（BLOCK_STREAM_SEND / BLOCK_STREAM_RECV）
 * @param[in]     timeout 阻塞超时 tick 数
 * @return pdPASS 成功挂起；pdFAIL 当前无有效任务（如 idle）
 * @retval pdPASS 已挂入等待队列
 * @retval pdFAIL 当前为 idle 或无 running 任务
 * @note 调用方须在 exit_critical 后 cgrtos_sched_yield
 * @warning 须在临界区内调用
 * @attention ❌ ISR；✅ block
 * @internal
 */
static int sb_block(cgrtos_stream_buffer_t *sb, cgrtos_task_t *volatile *wq,
                    block_reason_t reason, tick_t timeout)
{
    /* 1. 获取当前 CPU 与 running 任务 */
    uint8_t cpu = arch_cpu_id();
    cgrtos_task_t *cur = g_current[cpu];
    if (!cur || cur->id == 0) {
        return pdFAIL;
    }
    /* 2. 标记阻塞并挂入等待队列 */
    cgrtos_sched_block(cur, reason, sb, timeout);
    cgrtos_wait_list_add(wq, cur);
    return pdPASS;
}

/**
 * @brief 计算环形缓冲剩余可写字节数
 * @details 用总容量 size 减去已占用字节数 avail 得到空闲空间。
 * @param[in] sb StreamBuffer 指针
 * @return 剩余空间（size - avail）
 * @retval >=0 可写字节数
 * @note 纯计算，不修改状态
 * @warning 无
 * @attention ❌ ISR；❌ block
 * @internal
 */
static uint32_t sb_spaces(const cgrtos_stream_buffer_t *sb)
{
    return sb->size - sb->avail;
}

/**
 * @brief 向环形缓冲写入 n 字节并更新 head/avail
 * @details 逐字节拷贝到 buf[head]，head 取模回绕；累加 avail 反映新增占用。
 * @param[in,out] sb  StreamBuffer 指针
 * @param[in]     src 源数据
 * @param[in]     n   写入字节数
 * @return 无
 * @retval 无
 * @note 调用方须确保 n <= sb_spaces(sb)
 * @warning 须在临界区内调用
 * @attention ❌ ISR；❌ block
 * @internal
 */
static void sb_write_bytes(cgrtos_stream_buffer_t *sb, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        sb->buf[sb->head] = src[i];
        sb->head = (sb->head + 1U) % sb->size;
    }
    sb->avail += n;
}

/**
 * @brief 从环形缓冲读出 n 字节并更新 tail/avail
 * @details 逐字节从 buf[tail] 拷贝到 dst，tail 取模回绕；递减 avail 反映释放占用。
 * @param[in,out] sb  StreamBuffer 指针
 * @param[out]    dst 目标缓冲
 * @param[in]     n   读出字节数
 * @return 无
 * @retval 无
 * @note 调用方须确保 n <= sb->avail
 * @warning 须在临界区内调用
 * @attention ❌ ISR；❌ block
 * @internal
 */
static void sb_read_bytes(cgrtos_stream_buffer_t *sb, uint8_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = sb->buf[sb->tail];
        sb->tail = (sb->tail + 1U) % sb->size;
    }
    sb->avail -= n;
}

/**
 * @brief 按 trigger 水位唤醒等待接收的任务
 * @details 确定唤醒阈值（MessageBuffer 为 1；StreamBuffer 为 trigger，0 则视为 1）；循环检查 recv_wait_q，avail 达阈值则弹出最高优先级等待者并 unblock。
 * @param[in]     sb         StreamBuffer 指针
 * @param[in,out] need_yield 若非 NULL，唤醒时置 1 提示调用方 yield
 * @return 无
 * @retval 无
 * @note 可在任务或 ISR 临界区内调用
 * @warning 无
 * @attention ✅ ISR；❌ block
 * @internal
 */
static void sb_wake_recv(cgrtos_stream_buffer_t *sb, int *need_yield)
{
    while (sb->recv_wait_q) {
        /* 1. 计算接收唤醒阈值 */
        uint32_t need = sb->is_message ? 1U : sb->trigger;
        if (need == 0) {
            need = 1;
        }
        if (sb->avail < need) {
            break;
        }
        /* 2. 弹出并唤醒最高优先级接收等待者 */
        cgrtos_task_t *w = cgrtos_wait_list_pop_highest(&sb->recv_wait_q);
        if (!w) {
            break;
        }
        w->wake_ok = 1;
        cgrtos_sched_unblock(w);
        if (need_yield) {
            *need_yield = 1;
        }
    }
}

/**
 * @brief 在有剩余空间时唤醒等待发送的任务
 * @details 当 send_wait_q 非空且 sb_spaces > 0 时循环；弹出最高优先级发送等待者，置 wake_ok=1 并 unblock。
 * @param[in]     sb         StreamBuffer 指针
 * @param[in,out] need_yield 若非 NULL，唤醒时置 1 提示调用方 yield
 * @return 无
 * @retval 无
 * @note 可在任务或 ISR 临界区内调用
 * @warning 无
 * @attention ✅ ISR；❌ block
 * @internal
 */
static void sb_wake_send(cgrtos_stream_buffer_t *sb, int *need_yield)
{
    while (sb->send_wait_q && sb_spaces(sb) > 0) {
        cgrtos_task_t *w = cgrtos_wait_list_pop_highest(&sb->send_wait_q);
        if (!w) {
            break;
        }
        w->wake_ok = 1;
        cgrtos_sched_unblock(w);
        if (need_yield) {
            *need_yield = 1;
        }
    }
}

/**
 * @brief 创建 StreamBuffer 实例
 * @details 校验 size >= 2；临界区内在静态池 g_sbs 中找空闲槽；从堆分配环形缓冲并初始化 head/tail/avail/trigger/in_use。
 * @param[in] size    环形缓冲容量（至少 2 字节）
 * @param[in] trigger 接收唤醒水位；0 则默认为 1
 * @return 成功返回实例指针；失败返回 NULL
 * @retval 非 NULL 新创建的 StreamBuffer
 * @retval NULL     参数非法、池满或 malloc 失败
 * @note trigger 决定 recv 阻塞唤醒的最小 avail
 * @warning 无
 * @attention ❌ ISR；❌ block
 */
cgrtos_stream_buffer_t *cgrtos_stream_buffer_create(uint32_t size, uint32_t trigger)
{
    if (size < 2) {
        return 0;
    }
    cgrtos_enter_critical();
    for (uint32_t i = 0; i < CGRTOS_MAX_STREAM_BUFFER; i++) {
        if (!g_sbs[i].in_use) {
            /* 2. 分配环形缓冲 */
            uint8_t *buf = (uint8_t *)cgrtos_malloc(size);
            if (!buf) {
                cgrtos_exit_critical();
                return 0;
            }
            /* 3. 初始化实例 */
            memset(&g_sbs[i], 0, sizeof(g_sbs[i]));
            g_sbs[i].buf = buf;
            g_sbs[i].size = size;
            g_sbs[i].trigger = trigger ? trigger : 1;
            g_sbs[i].in_use = 1;
            cgrtos_exit_critical();
            return &g_sbs[i];
        }
    }
    cgrtos_exit_critical();
    return 0;
}

/**
 * @brief 查询缓冲内已有字节数
 * @details 若 sb 有效则返回 avail，否则返回 0。
 * @param[in] sb StreamBuffer 指针
 * @return avail 字节数；sb 为 NULL 时返回 0
 * @retval >=0 当前可用字节数
 * @note 只读查询，无锁（调用方须自行同步或接受竞态）
 * @warning 并发读写时返回值可能瞬时变化
 * @attention ❌ ISR；❌ block
 */
uint32_t cgrtos_stream_buffer_bytes_available(cgrtos_stream_buffer_t *sb)
{
    return sb ? sb->avail : 0;
}

/**
 * @brief 查询缓冲剩余可写字节数
 * @details 若 sb 有效则调用 sb_spaces，否则返回 0。
 * @param[in] sb StreamBuffer 指针
 * @return 剩余空间；sb 为 NULL 时返回 0
 * @retval >=0 可写字节数
 * @note 只读查询
 * @warning 并发读写时返回值可能瞬时变化
 * @attention ❌ ISR；❌ block
 */
uint32_t cgrtos_stream_buffer_spaces_available(cgrtos_stream_buffer_t *sb)
{
    return sb ? sb_spaces(sb) : 0;
}

/**
 * @brief 任务上下文向 StreamBuffer 发送数据（可部分写入）
 * @details 循环：临界区计算可写 n 并写入；全部写完或 timeout==0 则返回；否则阻塞到 send_wait_q 并 yield。
 * @param[in]     sb      StreamBuffer 指针
 * @param[in]     data    源数据
 * @param[in]     len     期望发送字节数
 * @param[in]     timeout 空间不足时的阻塞超时；0 表示不阻塞
 * @return 实际写入字节数
 * @retval >=0 实际写入字节数（可能小于 len）
 * @note 唤醒后 timeout 置 0，仅再尝试一次非阻塞写入
 * @warning 挂入 QueueSet 时写入成功会 poke
 * @attention ❌ ISR；✅ block
 */
size_t cgrtos_stream_buffer_send(cgrtos_stream_buffer_t *sb, const void *data,
                                 size_t len, tick_t timeout)
{
    /* 1. 参数校验 */
    if (!sb || !data || len == 0 || !sb->in_use) {
        return 0;
    }

    const uint8_t *src = (const uint8_t *)data;
    size_t written = 0;

    for (;;) {
        int need_yield = 0;
        cgrtos_enter_critical();
        uint8_t cpu = arch_cpu_id();
        cgrtos_task_t *cur = g_current[cpu];
        /* 2. 计算本次可写字节数 */
        uint32_t space = sb_spaces(sb);
        uint32_t n = (uint32_t)(len - written);
        if (n > space) {
            n = space;
        }
        if (n > 0) {
            /* 3. 写入并唤醒接收者 */
            sb_write_bytes(sb, src + written, n);
            written += n;
            sb_wake_recv(sb, &need_yield);
            if (sb->qset) {
                cgrtos_queue_set_poke(sb->qset, sb);
            }
        }
        /* 4. 全部写完则返回 */
        if (written >= len) {
            cgrtos_exit_critical();
            if (need_yield) {
                cgrtos_sched_yield();
            }
            return written;
        }
        /* 5. 非阻塞模式：返回已写部分 */
        if (timeout == 0) {
            cgrtos_exit_critical();
            if (need_yield) {
                cgrtos_sched_yield();
            }
            return written;
        }
        /* 6. 阻塞等待空间 */
        sb_block(sb, &sb->send_wait_q, BLOCK_STREAM_SEND, timeout);
        cgrtos_exit_critical();
        cgrtos_sched_yield();
        if (!cur || !cur->wake_ok) {
            return written;
        }
        timeout = 0;
    }
}

/**
 * @brief ISR 上下文向 StreamBuffer 发送数据（非阻塞，可部分写入）
 * @details 校验参数；临界区 n = min(len, 空闲空间)；写入并 sb_wake_recv；若挂接 QueueSet 则 poke；notify_woken。
 * @param[in]     sb    StreamBuffer 指针
 * @param[in]     data  源数据
 * @param[in]     len   期望发送字节数
 * @param[in,out] woken 可选 woken 标志
 * @return 实际写入字节数（0 表示参数非法、未初始化或无空间）
 * @retval >=0 实际写入字节数
 * @note 绝不阻塞
 * @warning 无空间时返回 0，已写部分保留
 * @attention ✅ ISR；❌ block
 */
size_t cgrtos_stream_buffer_send_from_isr(cgrtos_stream_buffer_t *sb, const void *data,
                                          size_t len, BaseType_t *woken)
{
    if (!sb || !data || len == 0 || !sb->in_use) {
        return 0;
    }
    int need_yield = 0;
    cgrtos_enter_critical();
    uint32_t space = sb_spaces(sb);
    uint32_t n = (uint32_t)len;
    if (n > space) {
        n = space;
    }
    if (n) {
        sb_write_bytes(sb, (const uint8_t *)data, n);
        sb_wake_recv(sb, &need_yield);
        if (sb->qset) {
            cgrtos_queue_set_poke(sb->qset, sb);
        }
    }
    cgrtos_exit_critical();
    cgrtos_isr_notify_woken(woken, need_yield);
    return n;
}

/**
 * @brief 任务上下文从 StreamBuffer 接收数据（可部分读取）
 * @details 循环：avail 达 trigger 或非阻塞且有数据则读取；否则阻塞到 recv_wait_q；超时则读取当前 avail 任意部分。
 * @param[in]  sb      StreamBuffer 指针
 * @param[out] buf     接收缓冲
 * @param[in]  len     期望读取字节数
 * @param[in]  timeout 数据不足时的阻塞超时；0 表示不阻塞
 * @return 实际读取字节数
 * @retval >=0 实际读取字节数
 * @note trigger 为 0 时视为 1
 * @warning 被唤醒后 timeout 置 0 再试一次
 * @attention ❌ ISR；✅ block
 */
size_t cgrtos_stream_buffer_recv(cgrtos_stream_buffer_t *sb, void *buf, size_t len,
                                 tick_t timeout)
{
    if (!sb || !buf || len == 0 || !sb->in_use) {
        return 0;
    }

    uint8_t *dst = (uint8_t *)buf;
    uint32_t trigger = sb->trigger ? sb->trigger : 1;

    for (;;) {
        int need_yield = 0;
        cgrtos_enter_critical();
        uint8_t cpu = arch_cpu_id();
        cgrtos_task_t *cur = g_current[cpu];

        /* 2. 数据达到唤醒水位或非阻塞且有数据 */
        if (sb->avail >= trigger || (sb->avail > 0 && timeout == 0)) {
            uint32_t n = sb->avail;
            if (n > (uint32_t)len) {
                n = (uint32_t)len;
            }
            sb_read_bytes(sb, dst, n);
            sb_wake_send(sb, &need_yield);
            cgrtos_exit_critical();
            if (need_yield) {
                cgrtos_sched_yield();
            }
            return n;
        }

        /* 4. 非阻塞且无足够数据 */
        if (timeout == 0) {
            cgrtos_exit_critical();
            return 0;
        }

        /* 5. 阻塞等待数据 */
        sb_block(sb, &sb->recv_wait_q, BLOCK_STREAM_RECV, timeout);
        cgrtos_exit_critical();
        cgrtos_sched_yield();
        if (!cur || !cur->wake_ok) {
            /* 6. 超时：读取当前可用部分 */
            cgrtos_enter_critical();
            uint32_t n = sb->avail;
            if (n > (uint32_t)len) {
                n = (uint32_t)len;
            }
            if (n) {
                sb_read_bytes(sb, dst, n);
                sb_wake_send(sb, &need_yield);
            }
            cgrtos_exit_critical();
            if (need_yield) {
                cgrtos_sched_yield();
            }
            return n;
        }
        timeout = 0;
    }
}

/**
 * @brief ISR 上下文从 StreamBuffer 接收数据（非阻塞，可部分读取）
 * @details 校验参数；临界区 n = min(avail, len)；若 n>0 则 sb_read_bytes 并 sb_wake_send；notify_woken。
 * @param[in]     sb    StreamBuffer 指针
 * @param[out]    buf   接收缓冲
 * @param[in]     len   最大读取字节数
 * @param[in,out] woken 可选 woken 标志
 * @return 实际读取字节数（0 表示空或参数非法）
 * @retval >=0 实际读取字节数
 * @note 绝不阻塞
 * @warning 无
 * @attention ✅ ISR；❌ block
 */
size_t cgrtos_stream_buffer_recv_from_isr(cgrtos_stream_buffer_t *sb, void *buf,
                                          size_t len, BaseType_t *woken)
{
    if (!sb || !buf || len == 0 || !sb->in_use) {
        return 0;
    }
    int need_yield = 0;
    cgrtos_enter_critical();
    uint32_t n = sb->avail;
    if (n > (uint32_t)len) {
        n = (uint32_t)len;
    }
    if (n) {
        sb_read_bytes(sb, (uint8_t *)buf, n);
        sb_wake_send(sb, &need_yield);
    }
    cgrtos_exit_critical();
    cgrtos_isr_notify_woken(woken, need_yield);
    return n;
}

/**
 * @brief 重置 StreamBuffer（清空数据，不释放内存）
 * @details 校验 sb 与 in_use；临界区内将 head/tail/avail 清零。
 * @param[in] sb StreamBuffer 指针
 * @return pdPASS 成功；pdFAIL 参数无效
 * @retval pdPASS 缓冲已清空
 * @retval pdFAIL sb 无效或未使用
 * @note 不唤醒或取消阻塞任务
 * @warning 重置后等待者仍阻塞直至超时或被 delete 唤醒
 * @attention ❌ ISR；❌ block
 */
int cgrtos_stream_buffer_reset(cgrtos_stream_buffer_t *sb)
{
    if (!sb || !sb->in_use) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    sb->head = sb->tail = sb->avail = 0;
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 删除 StreamBuffer 并释放资源
 * @details 临界区内唤醒所有 send/recv 等待者（wake_ok=0）；保存 buf 指针并清零实例；退出临界区后 free 缓冲并 yield。
 * @param[in] sb StreamBuffer 指针
 * @return pdPASS 成功；pdFAIL 参数无效
 * @retval pdPASS 实例已删除
 * @retval pdFAIL sb 无效或未使用
 * @note 被唤醒任务 wake_ok=0 表示失败/取消
 * @warning 删除后指针不可再使用
 * @attention ❌ ISR；✅ block
 */
int cgrtos_stream_buffer_delete(cgrtos_stream_buffer_t *sb)
{
    if (!sb || !sb->in_use) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    /* 2. 唤醒并取消所有发送等待者 */
    while (sb->send_wait_q) {
        cgrtos_task_t *w = cgrtos_wait_list_pop_highest(&sb->send_wait_q);
        if (w) {
            w->wake_ok = 0;
            cgrtos_sched_unblock(w);
        }
    }
    /* 2. 唤醒并取消所有接收等待者 */
    while (sb->recv_wait_q) {
        cgrtos_task_t *w = cgrtos_wait_list_pop_highest(&sb->recv_wait_q);
        if (w) {
            w->wake_ok = 0;
            cgrtos_sched_unblock(w);
        }
    }
    uint8_t *buf = sb->buf;
    memset(sb, 0, sizeof(*sb));
    cgrtos_exit_critical();
    if (buf) {
        cgrtos_free(buf);
    }
    cgrtos_sched_yield();
    return pdPASS;
}

/* ---- MessageBuffer: uint32_t length prefix + payload ---- */

/**
 * @brief 创建 MessageBuffer 实例
 * @details 以 trigger=1 创建底层 StreamBuffer；设置 is_message=1，确保按整消息语义唤醒接收者。
 * @param[in] size 环形缓冲总容量（需容纳 4 字节长度前缀 + 载荷）
 * @return 成功返回实例指针；失败返回 NULL
 * @retval 非 NULL 新创建的 MessageBuffer
 * @retval NULL     底层 StreamBuffer 创建失败
 * @note 与 StreamBuffer 共用 g_sbs 池
 * @warning 单条消息最大载荷 0xFFFF 字节
 * @attention ❌ ISR；❌ block
 */
cgrtos_message_buffer_t *cgrtos_message_buffer_create(uint32_t size)
{
    cgrtos_stream_buffer_t *sb = cgrtos_stream_buffer_create(size, 1);
    if (sb) {
        sb->is_message = 1;
        sb->trigger = 1;
    }
    return sb;
}

/**
 * @brief 向 MessageBuffer 发送一条完整消息
 * @details 校验 len <= 0xFFFF 且 need=4+len <= size；空间足够时原子写入 u32 长度前缀与 payload；否则阻塞或超时返回 0。
 * @param[in] mb      MessageBuffer 指针
 * @param[in] data    消息载荷
 * @param[in] len     载荷字节数（最大 0xFFFF）
 * @param[in] timeout 空间不足时的阻塞超时
 * @return 成功返回 len；失败或超时返回 0
 * @retval len 整包发送成功
 * @retval 0   参数非法、空间不足或超时
 * @note 绝不部分写入
 * @warning 无
 * @attention ❌ ISR；✅ block
 */
size_t cgrtos_message_buffer_send(cgrtos_message_buffer_t *mb, const void *data,
                                  size_t len, tick_t timeout)
{
    if (!mb || !data || len == 0 || len > 0xFFFFU) {
        return 0;
    }
    uint32_t need = (uint32_t)(sizeof(uint32_t) + len);
    if (need > mb->size) {
        return 0;
    }

    for (;;) {
        int need_yield = 0;
        cgrtos_enter_critical();
        uint8_t cpu = arch_cpu_id();
        cgrtos_task_t *cur = g_current[cpu];

        /* 2. 空间足够：写入长度前缀 + 载荷 */
        if (sb_spaces(mb) >= need) {
            uint32_t L = (uint32_t)len;
            sb_write_bytes(mb, (const uint8_t *)&L, sizeof(L));
            sb_write_bytes(mb, (const uint8_t *)data, (uint32_t)len);
            sb_wake_recv(mb, &need_yield);
            if (mb->qset) {
                cgrtos_queue_set_poke(mb->qset, mb);
            }
            cgrtos_exit_critical();
            if (need_yield) {
                cgrtos_sched_yield();
            }
            return len;
        }

        if (timeout == 0) {
            cgrtos_exit_critical();
            return 0;
        }
        sb_block(mb, &mb->send_wait_q, BLOCK_STREAM_SEND, timeout);
        cgrtos_exit_critical();
        cgrtos_sched_yield();
        if (!cur || !cur->wake_ok) {
            return 0;
        }
        timeout = 0;
    }
}

/**
 * @brief ISR 向 MessageBuffer 发送一条完整消息（非阻塞）
 * @details 校验参数与 need=4+len；空闲不足则返回 0；否则写 u32 前缀与 payload，唤醒接收者并 poke QueueSet。
 * @param[in]     mb    MessageBuffer 指针
 * @param[in]     data  消息载荷
 * @param[in]     len   载荷字节数（最大 0xFFFF）
 * @param[in,out] woken 可选 woken 标志
 * @return 成功返回 len；空间不足或参数非法返回 0（绝不部分写入）
 * @retval len 整包发送成功
 * @retval 0   失败或空间不足
 * @note 绝不部分写入
 * @warning 无
 * @attention ✅ ISR；❌ block
 */
size_t cgrtos_message_buffer_send_from_isr(cgrtos_message_buffer_t *mb, const void *data,
                                           size_t len, BaseType_t *woken)
{
    if (!mb || !data || len == 0 || len > 0xFFFFU || !mb->in_use) {
        return 0;
    }
    uint32_t need = (uint32_t)(sizeof(uint32_t) + len);
    if (need > mb->size) {
        return 0;
    }

    int need_yield = 0;
    cgrtos_enter_critical();
    if (sb_spaces(mb) < need) {
        cgrtos_exit_critical();
        return 0;
    }
    uint32_t L = (uint32_t)len;
    sb_write_bytes(mb, (const uint8_t *)&L, sizeof(L));
    sb_write_bytes(mb, (const uint8_t *)data, (uint32_t)len);
    sb_wake_recv(mb, &need_yield);
    if (mb->qset) {
        cgrtos_queue_set_poke(mb->qset, mb);
    }
    cgrtos_exit_critical();
    cgrtos_isr_notify_woken(woken, need_yield);
    return len;
}

/**
 * @brief ISR 从 MessageBuffer 接收一条完整消息（非阻塞）
 * @details avail < 4 或无完整消息（avail < 4+L）或 L > buf_len 均返回 0；否则消费前缀与载荷，唤醒发送者。
 * @param[in]     mb      MessageBuffer 指针
 * @param[out]    buf     接收缓冲
 * @param[in]     buf_len 接收缓冲容量
 * @param[in,out] woken   可选 woken 标志
 * @return 成功返回载荷长度；无完整消息、缓冲过小或参数非法返回 0
 * @retval >0 消息载荷字节数
 * @retval 0  无完整消息或失败
 * @note 窥读长度前缀时不移动 tail，消息未到齐不消费
 * @warning 无
 * @attention ✅ ISR；❌ block
 */
size_t cgrtos_message_buffer_recv_from_isr(cgrtos_message_buffer_t *mb, void *buf,
                                           size_t buf_len, BaseType_t *woken)
{
    if (!mb || !buf || buf_len == 0 || !mb->in_use) {
        return 0;
    }

    int need_yield = 0;
    cgrtos_enter_critical();
    if (mb->avail < sizeof(uint32_t)) {
        cgrtos_exit_critical();
        return 0;
    }
    uint32_t L = 0;
    uint32_t t = mb->tail;
    for (uint32_t i = 0; i < sizeof(uint32_t); i++) {
        ((uint8_t *)&L)[i] = mb->buf[t];
        t = (t + 1U) % mb->size;
    }
    if (mb->avail < sizeof(uint32_t) + L || L > buf_len) {
        cgrtos_exit_critical();
        return 0;
    }
    uint32_t discard;
    sb_read_bytes(mb, (uint8_t *)&discard, sizeof(discard));
    sb_read_bytes(mb, (uint8_t *)buf, L);
    sb_wake_send(mb, &need_yield);
    cgrtos_exit_critical();
    cgrtos_isr_notify_woken(woken, need_yield);
    return L;
}

/**
 * @brief 从 MessageBuffer 接收一条完整消息
 * @details 循环：avail >= 4 时 peek 长度 L；消息完整且 L <= buf_len 则读出；否则阻塞或超时返回 0。
 * @param[in]  mb      MessageBuffer 指针
 * @param[out] buf     接收缓冲
 * @param[in]  buf_len 接收缓冲容量
 * @param[in]  timeout 消息未完整到达时的阻塞超时
 * @return 成功返回消息载荷字节数；失败/超时/缓冲过小返回 0
 * @retval >0 消息载荷字节数
 * @retval 0  超时、缓冲不足或参数非法
 * @note 绝不部分接收
 * @warning 无
 * @attention ❌ ISR；✅ block
 */
size_t cgrtos_message_buffer_recv(cgrtos_message_buffer_t *mb, void *buf, size_t buf_len,
                                  tick_t timeout)
{
    if (!mb || !buf || buf_len == 0) {
        return 0;
    }

    for (;;) {
        int need_yield = 0;
        cgrtos_enter_critical();
        uint8_t cpu = arch_cpu_id();
        cgrtos_task_t *cur = g_current[cpu];

        if (mb->avail >= sizeof(uint32_t)) {
            /* 2. 窥读长度前缀（不移动 tail） */
            uint32_t L = 0;
            uint32_t t = mb->tail;
            for (uint32_t i = 0; i < sizeof(uint32_t); i++) {
                ((uint8_t *)&L)[i] = mb->buf[t];
                t = (t + 1U) % mb->size;
            }
            if (mb->avail < sizeof(uint32_t) + L) {
                /* 3. 消息未完整，继续等待 */
            } else if (L > buf_len) {
                /* 4. 调用方缓冲过小 */
                cgrtos_exit_critical();
                return 0;
            } else {
                /* 5. 消费前缀与载荷 */
                uint32_t discard;
                sb_read_bytes(mb, (uint8_t *)&discard, sizeof(discard));
                sb_read_bytes(mb, (uint8_t *)buf, L);
                sb_wake_send(mb, &need_yield);
                cgrtos_exit_critical();
                if (need_yield) {
                    cgrtos_sched_yield();
                }
                return L;
            }
        }

        if (timeout == 0) {
            cgrtos_exit_critical();
            return 0;
        }
        sb_block(mb, &mb->recv_wait_q, BLOCK_STREAM_RECV, timeout);
        cgrtos_exit_critical();
        cgrtos_sched_yield();
        if (!cur || !cur->wake_ok) {
            return 0;
        }
        timeout = 0;
    }
}

/**
 * @brief 删除 MessageBuffer 实例
 * @details 委托 cgrtos_stream_buffer_delete 释放底层 StreamBuffer 资源。
 * @param[in] mb MessageBuffer 指针
 * @return pdPASS 成功；pdFAIL 失败
 * @retval pdPASS 已删除
 * @retval pdFAIL 参数无效
 * @note MessageBuffer 与 StreamBuffer 共用删除路径
 * @warning 无
 * @attention ❌ ISR；✅ block
 */
int cgrtos_message_buffer_delete(cgrtos_message_buffer_t *mb)
{
    return cgrtos_stream_buffer_delete(mb);
}
