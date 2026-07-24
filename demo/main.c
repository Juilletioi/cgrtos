/**
 * @file main.c
 * @brief CG-RTOS 交互演示：生产者/消费者、心跳与 MC-EDF 周期任务
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * 通过 `make APP=demo` 构建。演示信号量、队列、事件组、软定时器与 SCHED_EDF。
 * 本文件为应用层示例，不导出内核 API。
 */

#include "../kernel/cgrtos.h"

/** @brief 演示用计数信号量（producer give / consumer take） */
static cgrtos_sem_t *sem;
/** @brief 演示用消息队列（uint32_t 元素） */
static cgrtos_queue_t *q;
/** @brief 演示用事件组 */
static cgrtos_event_group_t *eg;
/** @brief 生产次数累计（统计展示） */
static volatile uint32_t blinks;

/**
 * @brief 软定时器回调：打印当前系统 tick
 * @details 由定时器守护任务上下文调用，非 ISR。仅打印，不阻塞。
 * @param[in] arg 未使用；固定为 NULL
 * @return 无
 * @retval 无
 * @note 勿在回调中长时间占用 CPU
 * @warning 回调内禁止调用会永久阻塞的 API（本回调未阻塞）
 * @attention ❌ ISR；❌ 本回调不主动阻塞（但运行于任务上下文）
 * @internal
 */
static void timer_cb(void *arg)
{
    (void)arg;
    cgrtos_printf("  [DEMO] soft-timer tick=%lu\n", (unsigned long)cgrtos_get_ticks());
}

/**
 * @brief 生产者任务：周期投递事件、队列消息并释放信号量
 * @details 创建周期软定时器后循环：置事件位、队列发送、sem_give、延时 250ms。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 优先级高于 consumer，保证生产侧可抢占
 * @warning 队列满时 queue_send(portMAX_DELAY) 会阻塞
 * @attention ❌ ISR；✅ 会阻塞并引起调度
 * @internal
 */
static void producer(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    cgrtos_timer_t *tmr = cgrtos_timer_create("demo", timer_cb, 0,
                                              portMS_TO_TICK(400), 1);
    if (tmr) {
        cgrtos_timer_start(tmr);
    }

    while (1) {
        n++;
        blinks++;
        cgrtos_printf("  [DEMO] produce %u @%lu\n", n, (unsigned long)cgrtos_get_ticks());
        cgrtos_event_group_set(eg, 0x1);
        cgrtos_queue_send(q, &n, portMAX_DELAY);
        cgrtos_sem_give(sem);
        cgrtos_delay_ms(250);
    }
}

/**
 * @brief 消费者任务：按 sem → event → queue 顺序取数据
 * @details 无限等待信号量后等待事件位并接收队列消息，打印结果。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 依赖 producer 投递顺序，避免长期饿死
 * @warning 全部使用 portMAX_DELAY，资源未创建时会死等
 * @attention ❌ ISR；✅ 会阻塞并引起调度
 * @internal
 */
static void consumer(void *arg)
{
    (void)arg;
    while (1) {
        cgrtos_sem_take(sem, portMAX_DELAY);
        event_flags_t f = cgrtos_event_group_wait_bits(eg, 0x1, 1, 0, portMAX_DELAY);
        uint32_t v = 0;
        cgrtos_queue_receive(q, &v, portMAX_DELAY);
        cgrtos_printf("  [DEMO] consume %u flags=0x%x blinks=%u\n",
                      v, f, blinks);
    }
}

/**
 * @brief 心跳任务：每秒打印一次系统 tick
 * @details 无限循环：cgrtos_delay_ms(1000) 后打印当前 ticks，供观察调度存活。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 以 SCHED_RR、较低优先级创建，不抢占 producer/consumer
 * @warning 无
 * @attention ❌ ISR；✅ 会阻塞（delay_ms）并引起调度
 * @internal
 */
static void ticker(void *arg)
{
    (void)arg;
    while (1) {
        cgrtos_delay_ms(1000);
        cgrtos_printf("  [DEMO] heartbeat ticks=%lu\n",
                      (unsigned long)cgrtos_get_ticks());
    }
}

/**
 * @brief MC-EDF 周期演示任务：100ms 周期 beat
 * @details 设置 period/deadline 后以 cgrtos_delay_until 对齐释放，打印运行核与 deadline。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 依赖 CONFIG_USE_EDF；直接读 g_current 仅作演示（应用应避免依赖内核全局）
 * @warning 过载时可能错过 deadline，仍继续下一周期
 * @attention ❌ ISR；✅ 会阻塞（delay_until）并参与 MC-EDF 调度
 * @internal
 */
static void edf_beat(void *arg)
{
    (void)arg;
    uint8_t cpu = arch_cpu_id();
    cgrtos_task_t *self = g_current[cpu];
    tick_t period = portMS_TO_TICK(100);
    tick_t wake = cgrtos_get_ticks() + period;

    if (self) {
        cgrtos_task_set_period(self->id, period);
        cgrtos_task_set_deadline(self->id, wake);
    }

    while (1) {
        tick_t now = cgrtos_get_ticks();
        if (now < wake) {
            cgrtos_delay_until(&wake, period);
        } else {
            wake = now + period;
            if (self) {
                cgrtos_task_set_deadline(self->id, wake);
            }
        }
        cpu = arch_cpu_id();
        self = g_current[cpu];
        cgrtos_printf("  [DEMO][MC-EDF] beat @%lu dl=%lu cpu=%u\n",
                      (unsigned long)cgrtos_get_ticks(),
                      (unsigned long)(self ? self->deadline : 0),
                      (unsigned)cpu);
    }
}

/**
 * @brief 演示应用 hart0 入口
 * @details 初始化内核与 IPC 对象，创建 demo 任务后启动调度器（不返回）。
 * @param[in] hartid 当前 hart 编号（应为 0）
 * @param[in] fdt    设备树指针（本演示忽略）
 * @param[in] end    链接脚本堆末/DDR 末提示地址
 * @return 理论上返回 0；正常路径在 cgrtos_start 后不再返回
 * @retval 0 仅异常路径可能返回
 * @note 须先于次核业务逻辑完成对象创建
 * @warning IPC 创建失败未检查时后续任务可能死等
 * @attention ❌ ISR；✅ block/switch（cgrtos_start 后多任务）
 */
int main(int hartid, void *fdt, void *end)
{
    (void)fdt;
    (void)end;

    cgrtos_init();
    cgrtos_printf("  [BOOT] Hart %d - CG-RTOS demo (DDR end %p)\n", hartid, end);

    sem = cgrtos_sem_create(0, 8);
    q = cgrtos_queue_create(4, sizeof(uint32_t));
    eg = cgrtos_event_group_create();

    cgrtos_task_create("producer", producer, 0, 8, SCHED_PRIORITY);
    cgrtos_task_create("consumer", consumer, 0, 7, SCHED_PRIORITY);
    cgrtos_task_create("ticker", ticker, 0, 3, SCHED_RR);
    cgrtos_task_create("edfbeat", edf_beat, 0, 4, SCHED_EDF);

    cgrtos_start();
    return 0;
}

/**
 * @brief 次核 C 入口：进入从核调度循环
 * @details 转调 cgrtos_start_secondary，运行本核 idle 并参与 SMP 调度。
 * @param[in] hartid 本核编号（1..CONFIG_NUM_CORES-1）
 * @return 无（不返回）
 * @retval 无
 * @note 由启动代码在次核上调用
 * @warning 勿在 hart0 调用
 * @attention ❌ ISR；✅ 进入调度后可持续切换任务
 */
void secondary_main(int hartid)
{
    cgrtos_start_secondary(hartid);
}
