/**
 * @file cgrtos_config.h
 * @brief CG-RTOS 功能与容量配置（用户可覆盖 / 最小裁剪）
 * @author Cong Zhou / Juilletioi
 * @version 5.1.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 本文件集中全部 `CONFIG_*` / `CGRTOS_MAX_*` 开关。应用或板级可用：
 * - `-DCONFIG_xxx=0/1` 覆盖单项；
 * - `-DCGRTOS_CONFIG_MINIMAL=1` 启用最小裁剪预设（再可单项覆盖）；
 * - 或复制本文件到板级目录并 `-I` 优先包含。
 *
 * `cgrtos.h` 仅 `#include` 本头，不再内嵌调参宏。
 */
#ifndef CGRTOS_CONFIG_H
#define CGRTOS_CONFIG_H

/**
 * @brief 最小裁剪预设：关闭可选调试/扩展子系统，保留核心调度与 IPC
 * @details 在包含本头前定义 `CGRTOS_CONFIG_MINIMAL=1`，或由 Makefile PROFILE=minimal 注入。
 */
#ifdef CGRTOS_CONFIG_MINIMAL
#ifndef CONFIG_USE_EDF
#define CONFIG_USE_EDF              0
#endif
#ifndef CONFIG_USE_PREEMPT_THRESH
#define CONFIG_USE_PREEMPT_THRESH   0
#endif
#ifndef CONFIG_USE_DPCP
#define CONFIG_USE_DPCP             0
#endif
#ifndef CONFIG_SCHED_STATS
#define CONFIG_SCHED_STATS          0
#endif
#ifndef CONFIG_DETECT_DEADLOCK
#define CONFIG_DETECT_DEADLOCK      0
#endif
#ifndef CONFIG_ISR_API_GUARD
#define CONFIG_ISR_API_GUARD        0
#endif
#ifndef CONFIG_USE_KLOG
#define CONFIG_USE_KLOG             0
#endif
#ifndef CONFIG_USE_TRACE
#define CONFIG_USE_TRACE            0
#endif
#ifndef CONFIG_USE_OBJ_QUERY
#define CONFIG_USE_OBJ_QUERY        0
#endif
#ifndef CONFIG_USE_MPU
#define CONFIG_USE_MPU              0
#endif
#ifndef CONFIG_USE_VFS
#define CONFIG_USE_VFS              0
#endif
#ifndef CONFIG_CLI_FS
#define CONFIG_CLI_FS               0
#endif
#ifndef CONFIG_USE_TIMERS
#define CONFIG_USE_TIMERS           0
#endif
#ifndef CONFIG_USE_TASK_NOTIFICATIONS
#define CONFIG_USE_TASK_NOTIFICATIONS 0
#endif
#ifndef CONFIG_USE_HOOKS
#define CONFIG_USE_HOOKS            0
#endif
#ifndef CONFIG_IDLE_SLEEP_HOOK
#define CONFIG_IDLE_SLEEP_HOOK      0
#endif
#ifndef CONFIG_CHECK_STACK_OVERFLOW
#define CONFIG_CHECK_STACK_OVERFLOW 0
#endif
#ifndef CONFIG_STACK_CHECK_ON_TICK
#define CONFIG_STACK_CHECK_ON_TICK  0
#endif
#ifndef CONFIG_SMP_IDLE_STEAL
#define CONFIG_SMP_IDLE_STEAL       0
#endif
#ifndef CONFIG_IDLE_BUSY_PUMP
#define CONFIG_IDLE_BUSY_PUMP       1
#endif
#endif /* CGRTOS_CONFIG_MINIMAL */

/* -------------------------------------------------------------------------- */
/* Version & configuration                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 内核版本字符串
 * @details 与 cgrtos_stats_dump 等诊断输出一致。
 */
#ifndef CGRTOS_VERSION
#define CGRTOS_VERSION              "5.1.0"
#endif

/**
 * @brief 使能 SMP 代码路径
 * @details 1=编译多核调度与 IPI 路径；0=单核裁剪。
 */
#ifndef CONFIG_SMP_ENABLE
#define CONFIG_SMP_ENABLE           1
#endif

/**
 * @brief 逻辑 hart / 核数量
 * @details 须为 1、2 或 4；可由构建 -DCONFIG_NUM_CORES=N 覆盖。
 * @note Makefile CORES=N 或 cgrtos.sh --cores N 可覆盖；默认 2。
 */
#ifndef CONFIG_NUM_CORES
#define CONFIG_NUM_CORES            2
#endif
#if (CONFIG_NUM_CORES != 1) && (CONFIG_NUM_CORES != 2) && (CONFIG_NUM_CORES != 4)
#error "CONFIG_NUM_CORES must be 1, 2, or 4"
#endif

/** @brief 链接脚本预留的最大核栈槽（固定 4） */
#define CONFIG_MAX_CORES            4
/** @brief 每核启动栈字节数（与 cgrtos.lds / startup.S 一致） */
#ifndef CONFIG_CORE_STACK_BYTES
#define CONFIG_CORE_STACK_BYTES     0x4000
#endif
/** @brief 任务控制块池大小（含占用槽） */
#ifndef CONFIG_MAX_TASKS
#define CONFIG_MAX_TASKS            64
#endif
/** @brief 最高优先级编号（0..本宏） */
#ifndef CONFIG_MAX_PRIORITY
#define CONFIG_MAX_PRIORITY         31
#endif
/** @brief 系统节拍频率 (Hz) */
#ifndef CONFIG_TICK_RATE_HZ
#define CONFIG_TICK_RATE_HZ         1000
#endif
/** @brief SysTimer / mtime 时钟频率 (Hz) */
#ifndef CONFIG_TIMER_CLOCK_HZ
#define CONFIG_TIMER_CLOCK_HZ       1000000ULL
#endif
/** @brief RR/部分策略默认时间片（tick） */
#ifndef CONFIG_TIME_SLICE_TICKS
#define CONFIG_TIME_SLICE_TICKS     10
#endif
/** @brief Hybrid：>= 该优先级走 Priority 侧 */
#ifndef CONFIG_RT_PRIO_THRESHOLD
#define CONFIG_RT_PRIO_THRESHOLD    20
#endif
/** @brief CFS 时间片（tick） */
#ifndef CONFIG_CFS_SLICE_TICKS
#define CONFIG_CFS_SLICE_TICKS      5
#endif
/** @brief 软定时器时间轮槽数（须为 2 的幂） */
#ifndef CONFIG_TIMER_WHEEL_SLOTS
#define CONFIG_TIMER_WHEEL_SLOTS    256
#endif
/** @brief EDF 下次释放轮槽数（须为 2 的幂） */
#ifndef CONFIG_EDF_RELEASE_SLOTS
#define CONFIG_EDF_RELEASE_SLOTS    256
#endif

/**
 * @brief MC-EDF 相对固定优先级的抢占窗口（tick）
 * @details EDF deadline <= now + 本值时可抢占同核 Priority/Hybrid-RT。
 */
#ifndef CONFIG_MCEDF_PRIO_SLACK_TICKS
#define CONFIG_MCEDF_PRIO_SLACK_TICKS  1
#endif

/** @brief 编译 EDF / MC-EDF 路径；0 裁剪全局 EDF 链与释放轮 */
#ifndef CONFIG_USE_EDF
#define CONFIG_USE_EDF              1
#endif

/** @brief 抢占阈值：仅 prio > preempt_thresh 的任务可抢占当前任务 */
#ifndef CONFIG_USE_PREEMPT_THRESH
#define CONFIG_USE_PREEMPT_THRESH   1
#endif

/** @brief 互斥量 DPCP 截止期限/优先级天花板协议（EDF+FP） */
#ifndef CONFIG_USE_DPCP
#define CONFIG_USE_DPCP             1
#endif

/** @brief 调度延迟与 CPU 占用采样 */
#ifndef CONFIG_SCHED_STATS
#define CONFIG_SCHED_STATS          1
#endif

/** @brief 互斥锁等待环基础检测 */
#ifndef CONFIG_DETECT_DEADLOCK
#define CONFIG_DETECT_DEADLOCK      1
#endif

/** @brief 中断上下文禁止阻塞 API，返回 errISR 并触发钩子 */
#ifndef CONFIG_ISR_API_GUARD
#define CONFIG_ISR_API_GUARD        1
#endif

/** @brief 监控关中断/临界区时长并告警 */
#ifndef CONFIG_IRQ_DISABLE_MONITOR
#define CONFIG_IRQ_DISABLE_MONITOR  0
#endif

/** @brief 关中断/临界区超时时长告警阈值（微秒） */
#ifndef CONFIG_IRQ_DISABLE_WARN_US
#define CONFIG_IRQ_DISABLE_WARN_US  5000U
#endif

/** @brief 堆用户区尾部 redzone 金丝雀（越界写检测） */
#ifndef CONFIG_HEAP_REDZONE
#define CONFIG_HEAP_REDZONE         0
#endif

/** @brief EDF 就绪结构用最小堆（1）或有序链表（0） */
#ifndef CONFIG_USE_EDF_HEAP
#define CONFIG_USE_EDF_HEAP         0
#endif

/** @brief 内核分级日志 */
#ifndef CONFIG_USE_KLOG
#define CONFIG_USE_KLOG             1
#endif

/** @brief 默认日志级别（0=NONE..4=DEBUG） */
#ifndef CONFIG_LOG_LEVEL
#define CONFIG_LOG_LEVEL            3
#endif

/** @brief MPU/任务隔离接口桩（硬件适配前为软件桩） */
#ifndef CONFIG_USE_MPU
#define CONFIG_USE_MPU              0
#endif

/** @brief MPU 最大 region 数 */
#ifndef CONFIG_MPU_MAX_REGIONS
#define CONFIG_MPU_MAX_REGIONS      8
#endif

/** @brief 空闲低功耗入口钩子（与 idle_hook 分离） */
#ifndef CONFIG_IDLE_SLEEP_HOOK
#define CONFIG_IDLE_SLEEP_HOOK      1
#endif

/**
 * @brief 编译 VFS 挂载层（mount/umount/statfs/sync/mkfs 路由）
 * @details 1=启用 `kernel/vfs.c`；0 时 CLI FS 若仍开启则直接走 RAM FS API。
 */
#ifndef CONFIG_USE_VFS
#define CONFIG_USE_VFS              1
#endif

/**
 * @brief CLI 文件系统命令集（pwd/cd/ls/cat/…）
 * @details 仅影响 `APP=cli`；0=裁剪 `tests/cli_fs.c` 注册与帮助。
 */
#ifndef CONFIG_CLI_FS
#define CONFIG_CLI_FS               1
#endif

/** @brief 堆喷毒（调试）；0=关 */
#ifndef CONFIG_HEAP_POISON
#define CONFIG_HEAP_POISON          0
#endif

/** @brief 普通任务内嵌栈深度（64-bit 字） */
#ifndef CONFIG_TASK_STACK_WORDS
#define CONFIG_TASK_STACK_WORDS     256
#endif
/** @brief TLSF 堆字节数 */
#ifndef CONFIG_HEAP_SIZE
#define CONFIG_HEAP_SIZE            (256 * 1024)
#endif
/** @brief Idle 任务栈深度（字） */
#ifndef CONFIG_IDLE_STACK_WORDS
#define CONFIG_IDLE_STACK_WORDS     128
#endif
/** @brief 软定时器守护任务优先级 */
#ifndef CONFIG_TIMER_TASK_PRIO
#define CONFIG_TIMER_TASK_PRIO      (CONFIG_MAX_PRIORITY - 1)
#endif
/** @brief 定时器命令队列深度 */
#ifndef CONFIG_TIMER_QUEUE_LEN
#define CONFIG_TIMER_QUEUE_LEN      16
#endif
/** @brief 编译软定时器子系统 */
#ifndef CONFIG_USE_TIMERS
#define CONFIG_USE_TIMERS           1
#endif
/** @brief 编译任务通知 */
#ifndef CONFIG_USE_TASK_NOTIFICATIONS
#define CONFIG_USE_TASK_NOTIFICATIONS 1
#endif
/** @brief 栈金丝雀 / 高水位检测 */
#ifndef CONFIG_CHECK_STACK_OVERFLOW
#define CONFIG_CHECK_STACK_OVERFLOW 1
#endif
/** @brief 每 tick 对本核当前任务做金丝雀抽检（需 CONFIG_CHECK_STACK_OVERFLOW） */
#ifndef CONFIG_STACK_CHECK_ON_TICK
#define CONFIG_STACK_CHECK_ON_TICK  1
#endif
/** @brief 互斥量递归加锁上限（额外层数；总持有 = recursive+1） */
#ifndef CONFIG_MUTEX_MAX_RECURSIVE
#define CONFIG_MUTEX_MAX_RECURSIVE  64
#endif
/** @brief PLIC 可注册的最大中断源编号（含） */
#ifndef CONFIG_IRQ_MAX_SOURCES
#define CONFIG_IRQ_MAX_SOURCES      64
#endif
/** @brief PLIC 源优先级最大值（1..本宏；0=禁用该源） */
#ifndef CONFIG_IRQ_PRIORITY_MAX
#define CONFIG_IRQ_PRIORITY_MAX     7
#endif
/**
 * @brief 允许调用 FromISR 的最高中断优先级（分组上界）
 * @note 更高优先级中断可在 critical_from_isr 期间嵌套；FromISR 临界区抬高 PLIC threshold。
 */
#ifndef CONFIG_IRQ_SYSCALL_MAX_PRIO
#define CONFIG_IRQ_SYSCALL_MAX_PRIO 3
#endif
/** @brief 1=ISR 内允许更高优先级 MEIP 嵌套；0=关嵌套 */
#ifndef CONFIG_IRQ_NESTING
#define CONFIG_IRQ_NESTING          1
#endif
/** @brief 编译 idle/tick/malloc-fail 等钩子 */
#ifndef CONFIG_USE_HOOKS
#define CONFIG_USE_HOOKS            1
#endif
/**
 * @brief Idle 忙等推进 mtime（QEMU 防 WFI 卡死 SysTimer）
 * @note 真实硅片可改为 0 改用 WFI。
 */
#ifndef CONFIG_IDLE_BUSY_PUMP
#define CONFIG_IDLE_BUSY_PUMP       1
#endif
/** @brief Push 均衡启动的最小加权负载差 */
#ifndef CONFIG_LOAD_BALANCE_HYST
#define CONFIG_LOAD_BALANCE_HYST    3
#endif
/** @brief 两次 Push 尝试的间隔（tick） */
#ifndef CONFIG_LOAD_BALANCE_PERIOD
#define CONFIG_LOAD_BALANCE_PERIOD  16
#endif
/** @brief Steal：对端 READY 数达到此阈值才偷 */
#ifndef CONFIG_LB_STEAL_MIN
#define CONFIG_LB_STEAL_MIN         2
#endif
/** @brief 1=使能 Idle Steal；0=仅 Push */
#ifndef CONFIG_SMP_IDLE_STEAL
#define CONFIG_SMP_IDLE_STEAL       0
#endif
/** @brief 优先级任务权重系数：weight+=(prio+1)*scale */
#ifndef CONFIG_LB_PRIO_SCALE
#define CONFIG_LB_PRIO_SCALE        4
#endif
/** @brief CFS/Hybrid-CFS 基础权重 */
#ifndef CONFIG_LB_CFS_WEIGHT
#define CONFIG_LB_CFS_WEIGHT        3
#endif
/** @brief 新建任务放到当前最轻载核 */
#ifndef CONFIG_SMP_INITIAL_PLACE
#define CONFIG_SMP_INITIAL_PLACE    1
#endif

/** @brief 任务名最大长度（含 NUL 预算） */
#ifndef CGRTOS_TASK_NAME_MAX
#define CGRTOS_TASK_NAME_MAX        16
#endif
/** @brief 信号量对象池容量 */
#ifndef CGRTOS_MAX_SEM
#define CGRTOS_MAX_SEM              64
#endif
/** @brief 互斥量对象池容量 */
#ifndef CGRTOS_MAX_MUTEX
#define CGRTOS_MAX_MUTEX            32
#endif
/** @brief 队列对象池容量 */
#ifndef CGRTOS_MAX_QUEUE
#define CGRTOS_MAX_QUEUE            32
#endif
/** @brief 软定时器对象池容量 */
#ifndef CGRTOS_MAX_TIMER
#define CGRTOS_MAX_TIMER            32
#endif
/** @brief 事件组对象池容量 */
#ifndef CGRTOS_MAX_EVENT
#define CGRTOS_MAX_EVENT            16
#endif
/** @brief Stream/Message Buffer 池容量 */
#ifndef CGRTOS_MAX_STREAM_BUFFER
#define CGRTOS_MAX_STREAM_BUFFER    8
#endif
/** @brief QueueSet 池容量 */
#ifndef CGRTOS_MAX_QUEUE_SET
#define CGRTOS_MAX_QUEUE_SET        4
#endif
/** @brief 每个 QueueSet 最大成员数 */
#ifndef CGRTOS_QUEUE_SET_LENGTH
#define CGRTOS_QUEUE_SET_LENGTH     8
#endif
/** @brief RAM FS inode 数 */
#ifndef CGRTOS_FS_MAX_INODES
#define CGRTOS_FS_MAX_INODES        64
#endif
/** @brief 打开文件描述符数 */
#ifndef CGRTOS_FS_MAX_FD
#define CGRTOS_FS_MAX_FD            16
#endif
/** @brief 目录项名最大长度（含 NUL） */
#ifndef CGRTOS_FS_MAX_NAME
#define CGRTOS_FS_MAX_NAME          32
#endif
/** @brief 单文件最大字节数 */
#ifndef CGRTOS_FS_MAX_FILE_BYTES
#define CGRTOS_FS_MAX_FILE_BYTES    (32 * 1024)
#endif


/** @brief 简易内核 Trace 环形缓冲 */
#ifndef CONFIG_USE_TRACE
#define CONFIG_USE_TRACE            1
#endif
/** @brief Trace 记录条数（须为 2 的幂） */
#ifndef CONFIG_TRACE_ENTRIES
#define CONFIG_TRACE_ENTRIES        256
#endif
/** @brief 系统对象查询 / 导出 API */
#ifndef CONFIG_USE_OBJ_QUERY
#define CONFIG_USE_OBJ_QUERY        1
#endif
/** @brief 内存池最大数量 */
#ifndef CGRTOS_MAX_MEMPOOL
#define CGRTOS_MAX_MEMPOOL          8
#endif


#endif /* CGRTOS_CONFIG_H */
