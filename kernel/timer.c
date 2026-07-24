/**
 * @file timer.c
 * @brief 软件定时器与分层时间轮
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 256 槽分层定时器轮 + 守护任务 "Tmr Svc"。
 * tick ISR（cgrtos_timer_process_tick）仅向队列投递 TIMER_CMD_EXPIRE；
 * 回调在守护任务上下文执行（非 ISR）。删除时回收池槽位。
 */
#include "cgrtos.h"
#include <string.h>

/** @def TIMER_CMD_EXPIRE 定时器到期命令码 */
#define TIMER_CMD_EXPIRE  1U
/** @def TIMER_CMD_START ISR/任务投递的启动命令 */
#define TIMER_CMD_START   2U
/** @def TIMER_CMD_STOP 停止命令 */
#define TIMER_CMD_STOP    3U
/** @def TIMER_CMD_RESET 复位命令 */
#define TIMER_CMD_RESET   4U
/** @def TIMER_CMD_CHANGE 改周期命令（period 字段有效） */
#define TIMER_CMD_CHANGE  5U
/** @def TIMER_WHEEL_MASK 时间轮槽位掩码 */
#define TIMER_WHEEL_MASK  (CONFIG_TIMER_WHEEL_SLOTS - 1U)

/** @brief 时间轮槽位链表节点 */
typedef struct timer_wheel_entry {
    cgrtos_timer_t             *timer; /**< 关联定时器 */
    struct timer_wheel_entry   *next;  /**< 同槽后继 */
    uint32_t                    rounds; /**< 剩余轮转圈数 */
} timer_wheel_entry_t;

/** @brief 定时器对象池 */
static cgrtos_timer_t           g_timers[CGRTOS_MAX_TIMER];
/** @brief 定时器槽占用位图 */
static uint8_t                  g_timer_used_slot[CGRTOS_MAX_TIMER];
/** @brief 时间轮链表节点池 */
static timer_wheel_entry_t      g_wheel_pool[CGRTOS_MAX_TIMER];
/** @brief 已分配的时间轮节点数 */
static uint32_t                 g_wheel_pool_used;
/** @brief 时间轮各槽链表头 */
static timer_wheel_entry_t     *g_wheel[CONFIG_TIMER_WHEEL_SLOTS];
/** @brief 时间轮当前游标槽 */
static uint32_t                 g_wheel_cursor;

/** @brief 定时器守护任务命令队列 */
static cgrtos_queue_t          *g_timer_cmd_q;

/**
 * @brief 从定时器对象池分配一个空闲槽位
 * @details 线性扫描 g_timer_used_slot[] 寻找首个未占用索引；标记占用、memset 清零 g_timers[i] 后返回指针；池满返回 NULL。
 * @return 已清零的 cgrtos_timer_t 指针；池满返回 NULL
 * @retval 非 NULL 新分配的定时器对象
 * @retval NULL     对象池已满
 * @note 须在临界区内调用
 * @warning 返回指针生命周期至 timer_free_slot
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static cgrtos_timer_t *timer_alloc(void)
{
    for (uint32_t i = 0; i < CGRTOS_MAX_TIMER; i++) {
        if (!g_timer_used_slot[i]) {
            g_timer_used_slot[i] = 1;
            memset(&g_timers[i], 0, sizeof(g_timers[i]));
            return &g_timers[i];
        }
    }
    return 0;
}

/**
 * @brief 释放定时器对象池槽位并清零对象
 * @details timer 为 NULL 则直接返回；在 g_timers[] 中线性查找匹配地址，清除占用位并 memset 清零。
 * @param[in] timer 待释放的定时器指针（须来自 g_timers[]）
 * @return 无
 * @retval 无
 * @note 调用前须已从时间轮摘除
 * @warning 传入非池内指针时静默无操作
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static void timer_free_slot(cgrtos_timer_t *timer)
{
    if (!timer) {
        return;
    }
    for (uint32_t i = 0; i < CGRTOS_MAX_TIMER; i++) {
        if (&g_timers[i] == timer) {
            g_timer_used_slot[i] = 0;
            memset(&g_timers[i], 0, sizeof(g_timers[i]));
            return;
        }
    }
}

/**
 * @brief 从时间轮节点池分配或复用一个 wheel_entry
 * @details 先扫描已扩展区间内 timer==NULL 的节点作复用；若无且未达上限则扩展 g_wheel_pool_used；memset 清零后返回。
 * @return 已清零的 timer_wheel_entry_t 指针；池满返回 NULL
 * @retval 非 NULL 可用 wheel 节点
 * @retval NULL     节点池已满
 * @note 释放节点通过 memset 置 timer=NULL 供复用
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static timer_wheel_entry_t *wheel_entry_alloc(void)
{
    for (uint32_t i = 0; i < g_wheel_pool_used; i++) {
        if (!g_wheel_pool[i].timer) {
            memset(&g_wheel_pool[i], 0, sizeof(g_wheel_pool[i]));
            return &g_wheel_pool[i];
        }
    }
    if (g_wheel_pool_used >= CGRTOS_MAX_TIMER) {
        return 0;
    }
    timer_wheel_entry_t *entry = &g_wheel_pool[g_wheel_pool_used++];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

/**
 * @brief 从时间轮所有槽位的链表中移除指定定时器
 * @details 遍历 g_wheel 各槽链表，查找 entry->timer == timer 的节点；找到后摘除并 memset 清零 entry；未找到则幂等返回。
 * @param[in] timer 目标定时器指针
 * @return 无
 * @retval 无
 * @note 同一 timer 重复调用安全
 * @warning 须在临界区内调用
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static void wheel_remove_entry(cgrtos_timer_t *timer)
{
    for (uint32_t i = 0; i < CONFIG_TIMER_WHEEL_SLOTS; i++) {
        timer_wheel_entry_t *entry = g_wheel[i];
        timer_wheel_entry_t *prev = 0;
        while (entry) {
            if (entry->timer == timer) {
                if (prev) {
                    prev->next = entry->next;
                } else {
                    g_wheel[i] = entry->next;
                }
                memset(entry, 0, sizeof(*entry));
                return;
            }
            prev = entry;
            entry = entry->next;
        }
    }
}

/**
 * @brief 将定时器按剩余 tick 数插入分层时间轮
 * @details 先 wheel_remove_entry 防重复挂载；wheel_entry_alloc 获取节点；ticks 最小钳制为 1；计算 rounds 与 slot 后头插法链入 g_wheel[slot]。
 * @param[in] timer 目标定时器
 * @param[in] ticks 距到期剩余的 tick 数（0 会被提升为 1）
 * @return 无
 * @retval 无
 * @note 更新 timer->remain 为插入时的 ticks
 * @warning wheel_entry_alloc 失败时静默返回，定时器不会挂轮
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static void wheel_insert(cgrtos_timer_t *timer, tick_t ticks)
{
    /* 1. 先 wheel_remove_entry 确保同一 timer 不在轮上重复挂载 */
    wheel_remove_entry(timer);

    /* 2. wheel_entry_alloc 获取节点；失败则静默返回 */
    timer_wheel_entry_t *entry = wheel_entry_alloc();
    if (!entry) {
        return;
    }

    entry->timer = timer;
    /* 3. ticks 最小钳制为 1 */
    if (ticks == 0) {
        ticks = 1;
    }
    /* 3. 计算 rounds 与 slot */
    entry->rounds = (uint32_t)((ticks - 1U) / CONFIG_TIMER_WHEEL_SLOTS);
    uint32_t slot = (uint32_t)((g_wheel_cursor + ticks) & TIMER_WHEEL_MASK);
    /* 4. 头插法链入 g_wheel[slot]，更新 timer->remain */
    entry->next = g_wheel[slot];
    g_wheel[slot] = entry;
    timer->remain = ticks;
}

/**
 * @brief 定时器守护任务入口：阻塞接收命令并执行
 * @details 无限循环从 g_timer_cmd_q 阻塞接收 timer_cmd_t；EXPIRE 在任务上下文调用 cb；START/STOP/RESET/CHANGE 委托对应任务级 API。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 用户定时器回调在此任务栈上执行
 * @warning 回调内不得长时间阻塞
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void timer_daemon_entry(void *arg)
{
    (void)arg;
    timer_cmd_t cmd;

    while (1) {
        if (cgrtos_queue_receive(g_timer_cmd_q, &cmd, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (!cmd.timer) {
            continue;
        }
        switch (cmd.cmd) {
        case TIMER_CMD_EXPIRE:
            if (cmd.timer->cb) {
                cmd.timer->cb(cmd.timer->arg);
            }
            break;
        case TIMER_CMD_START:
            (void)cgrtos_timer_start(cmd.timer);
            break;
        case TIMER_CMD_STOP:
            (void)cgrtos_timer_stop(cmd.timer);
            break;
        case TIMER_CMD_RESET:
            (void)cgrtos_timer_reset(cmd.timer);
            break;
        case TIMER_CMD_CHANGE:
            (void)cgrtos_timer_change_period(cmd.timer, cmd.period);
            break;
        default:
            break;
        }
    }
}

/**
 * @brief ISR 向定时器守护队列投递一条控制命令
 * @details 校验 timer 与 g_timer_cmd_q；组装 timer_cmd_t 后委托 cgrtos_queue_send_from_isr；实际 start/stop 等在守护任务上下文执行。
 * @param[in]  timer  目标定时器（不得为 NULL）
 * @param[in]  cmd_id TIMER_CMD_START/STOP/RESET/CHANGE 之一
 * @param[in]  period CHANGE 命令的新周期；其它命令传 0
 * @param[out] woken  可选 woken（队列发送可能唤醒 Tmr Svc）
 * @return pdPASS 入队成功；pdFAIL 参数非法、队列未初始化或队列满
 * @retval pdPASS 命令已入队
 * @retval pdFAIL 参数非法或队列不可用
 * @note 不在 ISR 内直接改时间轮
 * @warning 队列满时命令丢失
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static int timer_cmd_from_isr(cgrtos_timer_t *timer, uint8_t cmd_id, tick_t period,
                              BaseType_t *woken)
{
    if (!timer || !g_timer_cmd_q) {
        return pdFAIL;
    }
    timer_cmd_t cmd = {
        .timer = timer,
        .cmd = cmd_id,
        .period = period,
    };
    return cgrtos_queue_send_from_isr(g_timer_cmd_q, &cmd, woken);
}

/**
 * @brief 初始化软件定时器子系统（时间轮、命令队列、守护任务）
 * @details 清零 g_wheel 各槽链表头，重置 cursor 与 wheel_pool_used；创建命令队列并 configASSERT 成功；创建 "Tmr Svc" 守护任务。
 * @return 无
 * @retval 无
 * @note 须在调度器启动前或早期 boot 阶段调用
 * @warning 重复 init 会泄漏旧队列/任务（当前未防重入）
 * @attention ❌ ISR；❌ block/switch
 */
void cgrtos_timer_init(void)
{
    memset(g_wheel, 0, sizeof(g_wheel));
    g_wheel_cursor = 0;
    g_wheel_pool_used = 0;

    g_timer_cmd_q = cgrtos_queue_create(CONFIG_TIMER_QUEUE_LEN, sizeof(timer_cmd_t));
    configASSERT(g_timer_cmd_q != 0);

    cgrtos_task_create("Tmr Svc", timer_daemon_entry, 0,
                       CONFIG_TIMER_TASK_PRIO, SCHED_PRIORITY);
}

/**
 * @brief 创建软件定时器对象（创建后处于停止状态）
 * @details 临界区内 timer_alloc 分配槽位；复制名称，设置 cb/arg/period/remain/periodic，active=0；退出临界区返回句柄。
 * @param[in] name     定时器名称（NULL 则默认 "timer"）
 * @param[in] cb       到期回调函数
 * @param[in] arg      回调用户参数
 * @param[in] period   周期或单次时长（tick）
 * @param[in] periodic 非零表示自动重装周期定时器
 * @return 定时器句柄；池满返回 NULL
 * @retval 非 NULL 新创建的定时器
 * @retval NULL     对象池已满
 * @note 创建后须 start 才挂入时间轮
 * @warning period 为 0 时 start 行为未定义
 * @attention ❌ ISR；❌ block/switch
 */
cgrtos_timer_t *cgrtos_timer_create(const char *name, timer_cb_t cb, void *arg,
                                    tick_t period, uint8_t periodic)
{
    cgrtos_enter_critical();

    cgrtos_timer_t *timer = timer_alloc();
    if (!timer) {
        cgrtos_exit_critical();
        return 0;
    }

    strncpy(timer->name, name ? name : "timer", CGRTOS_TASK_NAME_MAX - 1);
    timer->name[CGRTOS_TASK_NAME_MAX - 1] = 0;
    timer->cb = cb;
    timer->arg = arg;
    timer->period = period;
    timer->remain = period;
    timer->periodic = periodic;
    timer->active = 0;

    cgrtos_exit_critical();
    return timer;
}

/**
 * @brief 启动定时器，将其插入时间轮开始倒计时
 * @details 校验 timer 非空；临界区内置 active=1，按 timer->period 调用 wheel_insert；退出临界区返回 pdPASS。
 * @param[in] timer 定时器句柄
 * @return pdPASS 成功；timer 为 NULL 返回 pdFAIL
 * @retval pdPASS 定时器已启动
 * @retval pdFAIL timer 为空
 * @note 重复 start 会重新按 period 挂轮
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_timer_start(cgrtos_timer_t *timer)
{
    /* 1. 校验 timer 非空 */
    if (!timer) {
        return pdFAIL;
    }

    /* 2. 临界区内置 active=1，按 timer->period 调用 wheel_insert */
    cgrtos_enter_critical();
    timer->active = 1;
    wheel_insert(timer, timer->period);
    /* 3. 退出临界区返回 pdPASS */
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 停止定时器并从时间轮移除
 * @details 校验 timer 非空；临界区内 active=0，wheel_remove_entry 摘除节点；退出临界区返回 pdPASS。
 * @param[in] timer 定时器句柄
 * @return pdPASS 成功；timer 为 NULL 返回 pdFAIL
 * @retval pdPASS 定时器已停止
 * @retval pdFAIL timer 为空
 * @note 停止后 cb 仍保留，可再次 start
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_timer_stop(cgrtos_timer_t *timer)
{
    /* 1. 校验 timer 非空 */
    if (!timer) {
        return pdFAIL;
    }

    /* 2. 临界区内 active=0，wheel_remove_entry 摘除节点 */
    cgrtos_enter_critical();
    timer->active = 0;
    wheel_remove_entry(timer);
    /* 3. 退出临界区返回 pdPASS */
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 重置定时器（等价于 stop 后按原 period 重新 start）
 * @details 直接委托 cgrtos_timer_start，按当前 period 重新插入时间轮。
 * @param[in] timer 定时器句柄
 * @return 与 cgrtos_timer_start 相同
 * @retval pdPASS 复位成功
 * @retval pdFAIL timer 为空
 * @note 不修改 period 字段
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_timer_reset(cgrtos_timer_t *timer)
{
    return cgrtos_timer_start(timer);
}

/**
 * @brief 修改定时器周期；若已激活则立即按新周期重装时间轮
 * @details 校验 timer 与 period 有效；临界区更新 timer->period；若 active==1 则 wheel_insert 按新 period 重新挂载。
 * @param[in] timer  定时器句柄
 * @param[in] period 新周期 tick 数（须非零）
 * @return pdPASS 成功；timer 为空或 period==0 返回 pdFAIL
 * @retval pdPASS 周期已更新
 * @retval pdFAIL 参数非法
 * @note 未激活时仅更新 period，start 后生效
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_timer_change_period(cgrtos_timer_t *timer, tick_t period)
{
    if (!timer || period == 0) {
        return pdFAIL;
    }
    cgrtos_enter_critical();
    timer->period = period;
    if (timer->active) {
        wheel_insert(timer, period);
    }
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief ISR：请求启动定时器（投递 START 到 Tmr Svc，不阻塞）
 * @details 委托 timer_cmd_from_isr(TIMER_CMD_START)；守护任务收到后调用 cgrtos_timer_start 插入时间轮。
 * @param[in]  timer 定时器句柄
 * @param[out] woken 可选 woken 标志
 * @return pdPASS 命令入队成功；pdFAIL 失败
 * @retval pdPASS 命令已投递
 * @retval pdFAIL 入队失败
 * @note 不在 ISR 内直接改时间轮
 * @warning 队列满时启动请求丢失
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_timer_start_from_isr(cgrtos_timer_t *timer, BaseType_t *woken)
{
    return timer_cmd_from_isr(timer, TIMER_CMD_START, 0, woken);
}

/**
 * @brief ISR：请求停止定时器（投递 STOP）
 * @details 委托 timer_cmd_from_isr(TIMER_CMD_STOP)；守护任务调用 cgrtos_timer_stop 从时间轮摘除。
 * @param[in]  timer 定时器句柄
 * @param[out] woken 可选 woken 标志
 * @return pdPASS / pdFAIL
 * @retval pdPASS 命令已投递
 * @retval pdFAIL 入队失败
 * @note 停止操作异步完成
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_timer_stop_from_isr(cgrtos_timer_t *timer, BaseType_t *woken)
{
    return timer_cmd_from_isr(timer, TIMER_CMD_STOP, 0, woken);
}

/**
 * @brief ISR：请求复位定时器（投递 RESET，等价重新 start）
 * @details 委托 timer_cmd_from_isr(TIMER_CMD_RESET)；守护任务调用 cgrtos_timer_reset 按当前 period 重新挂轮。
 * @param[in]  timer 定时器句柄
 * @param[out] woken 可选 woken 标志
 * @return pdPASS / pdFAIL
 * @retval pdPASS 命令已投递
 * @retval pdFAIL 入队失败
 * @note 复位操作异步完成
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_timer_reset_from_isr(cgrtos_timer_t *timer, BaseType_t *woken)
{
    return timer_cmd_from_isr(timer, TIMER_CMD_RESET, 0, woken);
}

/**
 * @brief ISR：请求修改定时器周期（投递 CHANGE）
 * @details 校验 timer 与 period!=0；将 period 写入命令结构，投递 TIMER_CMD_CHANGE；守护任务调用 cgrtos_timer_change_period。
 * @param[in]  timer  定时器句柄
 * @param[in]  period 新周期 tick（须非 0）
 * @param[out] woken  可选 woken 标志
 * @return pdPASS 入队成功；pdFAIL 参数非法或队列满
 * @retval pdPASS 命令已投递
 * @retval pdFAIL 参数非法或队列满
 * @note 改周期操作异步完成
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cgrtos_timer_change_period_from_isr(cgrtos_timer_t *timer, tick_t period,
                                        BaseType_t *woken)
{
    if (!timer || period == 0) {
        return pdFAIL;
    }
    return timer_cmd_from_isr(timer, TIMER_CMD_CHANGE, period, woken);
}

/**
 * @brief 删除定时器，从时间轮移除并回收对象池槽位
 * @details 校验 timer 非空；临界区内 active=0、cb=0，wheel_remove_entry 摘除，timer_free_slot 归还槽位。
 * @param[in] timer 定时器句柄
 * @return pdPASS 成功；timer 为 NULL 返回 pdFAIL
 * @retval pdPASS 定时器已删除
 * @retval pdFAIL timer 为空
 * @note 删除后指针不可再使用
 * @warning 守护队列中待处理命令可能仍引用已删 timer
 * @attention ❌ ISR；❌ block/switch
 */
int cgrtos_timer_delete(cgrtos_timer_t *timer)
{
    if (!timer) {
        return pdFAIL;
    }

    cgrtos_enter_critical();
    timer->active = 0;
    timer->cb = 0;
    wheel_remove_entry(timer);
    timer_free_slot(timer);
    cgrtos_exit_critical();
    return pdPASS;
}

/**
 * @brief 每个系统 tick 由 ISR 调用，推进时间轮并投递到期事件到守护任务
 * @details g_timer_cmd_q 未初始化则返回；临界区内 cursor 前进，处理当前槽链表；rounds==0 时向队列 ISR 发送 EXPIRE；周期定时器重新 wheel_insert，单次 active=0。
 * @return 无
 * @retval 无
 * @note 用户回调在 "Tmr Svc" 守护任务上下文执行，非 ISR
 * @warning 须在 tick ISR 中调用，持临界区时间应尽可能短
 * @attention ✅ ISR；❌ block/switch
 */
void cgrtos_timer_process_tick(void)
{
    /* 1. g_timer_cmd_q 未初始化则直接返回 */
    if (!g_timer_cmd_q) {
        return;
    }

    cgrtos_enter_critical();

    /* 2. 临界区内 cursor 前进一格（&MASK），处理当前槽链表 */
    g_wheel_cursor = (g_wheel_cursor + 1U) & TIMER_WHEEL_MASK;
    timer_wheel_entry_t *entry = g_wheel[g_wheel_cursor];
    timer_wheel_entry_t *prev = 0;

    while (entry) {
        if (entry->rounds > 0) {
            /* 3. rounds>0 则递减并跳过（尚未到期） */
            entry->rounds--;
            prev = entry;
            entry = entry->next;
            continue;
        }

        /* 3. rounds==0 则该 entry 到期 */
        cgrtos_timer_t *timer = entry->timer;
        timer_wheel_entry_t *expired = entry;
        if (prev) {
            prev->next = entry->next;
        } else {
            g_wheel[g_wheel_cursor] = entry->next;
        }
        entry = entry->next;

        if (timer && timer->active) {
            timer_cmd_t cmd = {
                .timer = timer,
                .cmd = TIMER_CMD_EXPIRE,
            };
            /* 4. 到期时向命令队列 ISR 发送 TIMER_CMD_EXPIRE（不在 ISR 调 cb） */
            cgrtos_queue_send_from_isr(g_timer_cmd_q, &cmd, 0);

            /* 5. 周期定时器重新 wheel_insert；单次定时器 active=0 */
            if (timer->periodic) {
                wheel_insert(timer, timer->period);
            } else {
                timer->active = 0;
            }
        }
        /* 6. 清零 expired entry 节点供池复用 */
        memset(expired, 0, sizeof(*expired));
    }

    cgrtos_exit_critical();
}

/**
 * @brief 统计已占用的软定时器槽数
 * @details 扫描 g_timer_used_slot；供对象查询使用。
 * @return 占用数
 * @retval >=0 数量
 * @note CONFIG_USE_TIMERS 路径
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
uint32_t cgrtos_timer_count_used(void)
{
    uint32_t n = 0;
#if CONFIG_USE_TIMERS
    cgrtos_enter_critical();
    for (uint32_t i = 0; i < CGRTOS_MAX_TIMER; i++) {
        if (g_timer_used_slot[i]) {
            n++;
        }
    }
    cgrtos_exit_critical();
#else
    (void)n;
#endif
    return n;
}
