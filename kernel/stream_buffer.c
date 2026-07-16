/**
 * @file stream_buffer.c
 * @brief StreamBuffer（字节流）与 MessageBuffer（长度前缀整消息）模块
 *
 * ## 模块设计
 *
 * 本模块提供两类 IPC 缓冲：
 *
 * - **StreamBuffer**：环形字节缓冲，支持部分读写；接收端通过 `trigger` 控制
 *   唤醒水位（可用字节数达到 trigger 才唤醒等待接收的任务）。
 * - **MessageBuffer**：在 StreamBuffer 之上封装，每条消息格式为
 *   `[u32 LE len][payload...]`，send/recv 必须整包操作。
 *
 * ## 同步与阻塞
 *
 * - 全局静态池 `g_sbs[]` 管理实例；创建时从 TLSF 堆分配环形缓冲。
 * - 发送/接收各自维护优先级等待队列（`send_wait_q` / `recv_wait_q`），
 *   阻塞时调用 `cgrtos_sched_block` 并挂入对应队列。
 * - 写入后若满足接收唤醒条件则 `sb_wake_recv`；读出后若腾出空间则 `sb_wake_send`。
 * - 若实例挂入 QueueSet，发送成功后调用 `cgrtos_queue_set_poke` 通知 select 等待者。
 *
 * @see cgrtos_stream
 */
#include "cgrtos.h"
#include <string.h>

static cgrtos_stream_buffer_t g_sbs[CGRTOS_MAX_STREAM_BUFFER];

/**
 * @brief 将当前任务挂到指定等待队列并标记阻塞原因
 *
 * @param sb      关联的 StreamBuffer 实例
 * @param wq      目标等待队列指针（send 或 recv）
 * @param reason  阻塞原因枚举（BLOCK_STREAM_SEND / BLOCK_STREAM_RECV）
 * @param timeout 阻塞超时 tick 数
 *
 * @return pdPASS 成功挂起；pdFAIL 当前无有效任务（如 idle）
 *
 * @details
 * 1. 读取当前 hart 的 running 任务指针。
 * 2. 若任务为空或为 idle（id==0），无法阻塞，返回 pdFAIL。
 * 3. 调用调度器将该任务置为 BLOCKED 并记录 reason、关联对象与 timeout。
 * 4. 将任务按优先级插入 wq 等待链表。
 * 5. 返回 pdPASS，调用方需在 exit_critical 后 yield。
 */
static int sb_block(cgrtos_stream_buffer_t *sb, cgrtos_task_t *volatile *wq,
                    block_reason_t reason, tick_t timeout)
{
    /* 1. 获取当前 CPU 与 running 任务 */
    uint8_t cpu = (uint8_t)read_csr(mhartid);
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
 *
 * @param sb StreamBuffer 指针
 * @return 剩余空间（size - avail）
 *
 * @details
 * 1. 用总容量减去已占用字节数得到空闲空间。
 */
static uint32_t sb_spaces(const cgrtos_stream_buffer_t *sb)
{
    return sb->size - sb->avail;
}

/**
 * @brief 向环形缓冲写入 n 字节并更新 head/avail
 *
 * @param sb  StreamBuffer 指针
 * @param src 源数据
 * @param n   写入字节数
 *
 * @details
 * 1. 逐字节拷贝到 buf[head]，head 取模回绕。
 * 2. 累加 avail 反映新增占用。
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
 *
 * @param sb  StreamBuffer 指针
 * @param dst 目标缓冲
 * @param n   读出字节数
 *
 * @details
 * 1. 逐字节从 buf[tail] 拷贝到 dst，tail 取模回绕。
 * 2. 递减 avail 反映释放占用。
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
 *
 * @param sb         StreamBuffer 指针
 * @param need_yield 若非 NULL，唤醒时置 1 提示调用方 yield
 *
 * @details
 * 1. 确定唤醒阈值：MessageBuffer 为 1 字节；StreamBuffer 为 trigger（0 则视为 1）。
 * 2. 循环检查 recv_wait_q，若 avail 未达阈值则停止。
 * 3. 弹出最高优先级等待者，置 wake_ok=1 并 unblock。
 * 4. 重复直到队列空或数据不足。
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
 *
 * @param sb         StreamBuffer 指针
 * @param need_yield 若非 NULL，唤醒时置 1 提示调用方 yield
 *
 * @details
 * 1. 当 send_wait_q 非空且 sb_spaces > 0 时循环。
 * 2. 弹出最高优先级发送等待者，置 wake_ok=1 并 unblock。
 * 3. 重复直到队列空或无空间。
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
 *
 * @param size    环形缓冲容量（至少 2 字节）
 * @param trigger 接收唤醒水位；0 则默认为 1
 * @return 成功返回实例指针；失败返回 NULL
 *
 * @details
 * 1. 校验 size >= 2。
 * 2. 进入临界区，在静态池 g_sbs 中找空闲槽。
 * 3. 从堆分配 size 字节环形缓冲；失败则返回 NULL。
 * 4. 初始化 head/tail/avail/trigger/in_use 等字段。
 * 5. 退出临界区并返回指针；池满则返回 NULL。
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
 *
 * @param sb StreamBuffer 指针
 * @return avail 字节数；sb 为 NULL 时返回 0
 *
 * @details
 * 1. 若 sb 有效则返回 avail，否则返回 0。
 */
uint32_t cgrtos_stream_buffer_bytes_available(cgrtos_stream_buffer_t *sb)
{
    return sb ? sb->avail : 0;
}

/**
 * @brief 查询缓冲剩余可写字节数
 *
 * @param sb StreamBuffer 指针
 * @return 剩余空间；sb 为 NULL 时返回 0
 *
 * @details
 * 1. 若 sb 有效则调用 sb_spaces，否则返回 0。
 */
uint32_t cgrtos_stream_buffer_spaces_available(cgrtos_stream_buffer_t *sb)
{
    return sb ? sb_spaces(sb) : 0;
}

/**
 * @brief 任务上下文向 StreamBuffer 发送数据（可部分写入）
 *
 * @param sb      StreamBuffer 指针
 * @param data    源数据
 * @param len     期望发送字节数
 * @param timeout 空间不足时的阻塞超时；0 表示不阻塞
 * @return 实际写入字节数
 *
 * @details
 * 1. 校验 sb/data/len/in_use。
 * 2. 循环：进入临界区，计算本次可写 n = min(剩余需求, 空闲空间)。
 * 3. 若 n>0：写入、唤醒接收者、若挂入 QueueSet 则 poke。
 * 4. 若已全部写完，退出并 yield（若需要），返回 written。
 * 5. 若 timeout==0 且未写完，立即返回已写部分。
 * 6. 否则阻塞到 send_wait_q，yield 后检查 wake_ok；超时则返回已写部分。
 * 7. 被唤醒后将 timeout 置 0，仅再尝试一次非阻塞写入。
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
        uint8_t cpu = (uint8_t)read_csr(mhartid);
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
 *
 * @param sb   StreamBuffer 指针
 * @param data 源数据
 * @param len  期望发送字节数
 * @return 实际写入字节数
 *
 * @details
 * 1. 校验参数与 in_use。
 * 2. 进入临界区，计算 n = min(len, 空闲空间)。
 * 3. 若 n>0：写入、唤醒接收者、必要时 poke QueueSet。
 * 4. 退出临界区；若唤醒了更高优先级任务则 yield_from_isr。
 * 5. 返回实际写入 n。
 */
size_t cgrtos_stream_buffer_send_from_isr(cgrtos_stream_buffer_t *sb, const void *data,
                                          size_t len)
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
    if (need_yield) {
        cgrtos_sched_yield_from_isr();
    }
    return n;
}

/**
 * @brief 任务上下文从 StreamBuffer 接收数据（可部分读取）
 *
 * @param sb      StreamBuffer 指针
 * @param buf     接收缓冲
 * @param len     期望读取字节数
 * @param timeout 数据不足时的阻塞超时；0 表示不阻塞
 * @return 实际读取字节数
 *
 * @details
 * 1. 校验参数；trigger 为 0 时视为 1。
 * 2. 循环：进入临界区，若 avail >= trigger 或（avail>0 且 timeout==0）则读取。
 * 3. 读取 n = min(avail, len)，更新 tail/avail，唤醒发送等待者，返回 n。
 * 4. 若 timeout==0 且无足够数据，返回 0。
 * 5. 否则阻塞到 recv_wait_q，yield 后检查 wake_ok。
 * 6. 超时则尝试读取当前 avail 的任意部分（可能少于 trigger）并返回。
 * 7. 被唤醒后将 timeout 置 0，再试一次。
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
        uint8_t cpu = (uint8_t)read_csr(mhartid);
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
 *
 * @param sb  StreamBuffer 指针
 * @param buf 接收缓冲
 * @param len 最大读取字节数
 * @return 实际读取字节数
 *
 * @details
 * 1. 校验参数。
 * 2. 进入临界区，n = min(avail, len)。
 * 3. 若 n>0：读出、唤醒发送等待者。
 * 4. 退出临界区；必要时 yield_from_isr。
 * 5. 返回 n。
 */
size_t cgrtos_stream_buffer_recv_from_isr(cgrtos_stream_buffer_t *sb, void *buf,
                                          size_t len)
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
    if (need_yield) {
        cgrtos_sched_yield_from_isr();
    }
    return n;
}

/**
 * @brief 重置 StreamBuffer（清空数据，不释放内存）
 *
 * @param sb StreamBuffer 指针
 * @return pdPASS 成功；pdFAIL 参数无效
 *
 * @details
 * 1. 校验 sb 与 in_use。
 * 2. 临界区内将 head/tail/avail 清零。
 * 3. 返回 pdPASS。
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
 *
 * @param sb StreamBuffer 指针
 * @return pdPASS 成功；pdFAIL 参数无效
 *
 * @details
 * 1. 校验 sb 与 in_use。
 * 2. 临界区内唤醒所有 send/recv 等待者（wake_ok=0 表示失败/取消）。
 * 3. 保存 buf 指针，清零实例结构。
 * 4. 退出临界区后 free 环形缓冲。
 * 5. yield 让被唤醒任务运行，返回 pdPASS。
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
 *
 * @param size 环形缓冲总容量（需容纳 4 字节长度前缀 + 载荷）
 * @return 成功返回实例指针；失败返回 NULL
 *
 * @details
 * 1. 以 trigger=1 创建底层 StreamBuffer。
 * 2. 设置 is_message=1，确保按整消息语义唤醒接收者。
 * 3. 返回实例指针。
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
 *
 * @param mb      MessageBuffer 指针
 * @param data    消息载荷
 * @param len     载荷字节数（最大 0xFFFF）
 * @param timeout 空间不足时的阻塞超时
 * @return 成功返回 len；失败或超时返回 0
 *
 * @details
 * 1. 校验 mb/data/len；len 不得超过 0xFFFF；need = 4 + len 不得超过 size。
 * 2. 循环：进入临界区，若 sb_spaces >= need 则原子写入。
 * 3. 先写 u32 小端长度前缀，再写 payload；唤醒接收者并 poke QueueSet。
 * 4. 若 timeout==0 且空间不足，返回 0。
 * 5. 否则阻塞到 send_wait_q；超时返回 0；唤醒后再试（timeout 置 0）。
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
        uint8_t cpu = (uint8_t)read_csr(mhartid);
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
 * @brief 从 MessageBuffer 接收一条完整消息
 *
 * @param mb      MessageBuffer 指针
 * @param buf     接收缓冲
 * @param buf_len 接收缓冲容量
 * @param timeout 消息未完整到达时的阻塞超时
 * @return 成功返回消息载荷字节数；失败/超时/缓冲过小返回 0
 *
 * @details
 * 1. 校验 mb/buf/buf_len。
 * 2. 循环：进入临界区，若 avail >= 4 则 peek 长度前缀 L（不消费）。
 * 3. 若 avail < 4+L，消息未完整，继续等待或超时返回 0。
 * 4. 若 L > buf_len，调用方缓冲不足，返回 0。
 * 5. 否则读出 4 字节前缀与 L 字节 payload，唤醒发送者，返回 L。
 * 6. timeout==0 且无完整消息则返回 0；否则阻塞到 recv_wait_q。
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
        uint8_t cpu = (uint8_t)read_csr(mhartid);
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
 *
 * @param mb MessageBuffer 指针
 * @return pdPASS 成功；pdFAIL 失败
 *
 * @details
 * 1. 委托 cgrtos_stream_buffer_delete 释放底层 StreamBuffer 资源。
 */
int cgrtos_message_buffer_delete(cgrtos_message_buffer_t *mb)
{
    return cgrtos_stream_buffer_delete(mb);
}
