#ifndef CGRTOS_H
#define CGRTOS_H

/**
 * @file cgrtos.h
 * @brief CG-RTOS 公共 API 头文件
 * @author Cong Zhou / Juilletioi
 * @version 5.1.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * FreeRTOS 风格命名；支持 SMP 多核与多种调度策略（PRIORITY / RR / CFS /
 * MC-EDF / HYBRID）及加权负载均衡。hart0：main → cgrtos_init → 创建任务 →
 * cgrtos_start；次核：secondary_main → cgrtos_start_secondary。
 * 外设经 HAL（hal/hal.h）抽象；cgrtos_uart_* 等为兼容层。
 */

#include <stdint.h>
#include <stddef.h>
#include "list.h"
#include "hal/hal.h"

/* -------------------------------------------------------------------------- */
/* Version & configuration — see cgrtos_config.h                              */
/* -------------------------------------------------------------------------- */
#include "cgrtos_config.h"

/** @brief 成功（FreeRTOS 兼容） */
#define pdPASS                      0
/** @brief 通用失败 */
#define pdFAIL                      (-1)
/** @brief 布尔真 */
#define pdTRUE                      1
/** @brief 布尔假 */
#define pdFALSE                     0

/**
 * @brief FreeRTOS 风格整型
 * @details 用于 FromISR woken 标志等。
 */
typedef int                         BaseType_t;

/** @brief 队列空 */
#define errQUEUE_EMPTY              (-1)
/** @brief 队列满 */
#define errQUEUE_FULL               (-2)
/** @brief 阻塞超时 */
#define errTIMEOUT                  (-3)
/** @brief 参数非法 */
#define errPARAM                    (-4)
/** @brief 资源/内存不足 */
#define errNO_MEM                   (-5)
/** @brief 中断上下文禁止调用 */
#define errISR                      (-6)
/** @brief 死锁检测命中 */
#define errDEADLOCK                 (-7)
/** @brief 溢出/二次释放等 */
#define errOVERFLOW                 (-8)
/** @brief 对象状态非法（重复删除等） */
#define errSTATE                    (-9)

/** @brief 关闭日志 */
#define CGRTOS_LOG_NONE             0
/** @brief ERROR 级别 */
#define CGRTOS_LOG_ERROR            1
/** @brief WARN 级别 */
#define CGRTOS_LOG_WARN             2
/** @brief INFO 级别 */
#define CGRTOS_LOG_INFO             3
/** @brief DEBUG 级别 */
#define CGRTOS_LOG_DEBUG            4

#if CONFIG_USE_KLOG
/**
 * @brief 输出 ERROR 级日志
 * @param tag 模块标签字符串
 * @param msg 消息字符串
 * @warning 可能调用 UART/console，非 ISR 安全。
 */
#define CGRTOS_LOGE(tag, msg) cgrtos_log(CGRTOS_LOG_ERROR, (tag), (msg))
/** @brief 输出 WARN 级日志 @param tag 模块标签 @param msg 消息 @warning 非 ISR 安全 */
#define CGRTOS_LOGW(tag, msg) cgrtos_log(CGRTOS_LOG_WARN, (tag), (msg))
/** @brief 输出 INFO 级日志 @param tag 模块标签 @param msg 消息 @warning 非 ISR 安全 */
#define CGRTOS_LOGI(tag, msg) cgrtos_log(CGRTOS_LOG_INFO, (tag), (msg))
/** @brief 输出 DEBUG 级日志 @param tag 模块标签 @param msg 消息 @warning 非 ISR 安全 */
#define CGRTOS_LOGD(tag, msg) cgrtos_log(CGRTOS_LOG_DEBUG, (tag), (msg))
#else
/** @brief CONFIG_USE_KLOG=0 时空操作 @param tag 忽略 @param msg 忽略 */
#define CGRTOS_LOGE(tag, msg) ((void)0)
#define CGRTOS_LOGW(tag, msg) ((void)0)
#define CGRTOS_LOGI(tag, msg) ((void)0)
#define CGRTOS_LOGD(tag, msg) ((void)0)
#endif

/** @brief 系统节拍计数类型 */
typedef uint64_t                    tick_t;
/** @brief 任务 ID（0 表示空闲槽/无效） */
typedef uint64_t                    task_id_t;
/** @brief 简易自旋锁（0=空闲，非 0=占用） */
typedef volatile uint32_t           spinlock_t;
/** @brief 事件组位图 */
typedef uint32_t                    event_flags_t;
/** @brief 任务入口函数 */
typedef void (*task_func_t)(void *);
/** @brief 软定时器回调 */
typedef void (*timer_cb_t)(void *);

/**
 * @brief 任务内部状态机
 */
typedef enum {
    TASK_READY = 0,   ///< 就绪，位于就绪队列
    TASK_RUNNING,     ///< 正在某核上运行
    TASK_BLOCKED,     ///< 阻塞（延时/IPC/通知）
    TASK_SUSPENDED,   ///< 被挂起
    TASK_TERMINATED,  ///< 已退出，等待回收为 DELETED
    TASK_DELETED      ///< 已删除，槽可复用
} task_state_t;

/**
 * @brief 调度策略
 */
typedef enum {
    SCHED_RR = 0,     ///< 同优先级时间片轮转
    SCHED_PRIORITY,   ///< 纯优先级，同优先级粘滞
    SCHED_CFS,        ///< 完全公平调度（vruntime）
    SCHED_EDF,        ///< 全局 MC-EDF（G-EDF）
    SCHED_HYBRID      ///< 高优先级 Priority，否则 CFS
} sched_policy_t;

/**
 * @brief 阻塞原因（配合 block_obj）
 */
typedef enum {
    BLOCK_NONE = 0,     ///< 未阻塞
    BLOCK_DELAY,        ///< 延时
    BLOCK_SEM,          ///< 信号量
    BLOCK_MUTEX,        ///< 互斥量
    BLOCK_QUEUE_SEND,   ///< 队列发送端
    BLOCK_QUEUE_RECV,   ///< 队列接收端
    BLOCK_EVENT,        ///< 事件组
    BLOCK_NOTIFY,       ///< 任务通知
    BLOCK_STREAM_SEND,  ///< StreamBuffer 发送
    BLOCK_STREAM_RECV,  ///< StreamBuffer 接收
    BLOCK_QUEUE_SET     ///< QueueSet select
} block_reason_t;

/**
 * @brief FreeRTOS 风格对外任务状态
 */
typedef enum {
    eReady = 0,       ///< 就绪
    eRunning,         ///< 运行中
    eBlocked,         ///< 阻塞
    eSuspended,       ///< 挂起
    eDeleted,         ///< 已删除
    eTerminated,      ///< 已终止
    eInvalid          ///< 无效 ID 或未知
} eTaskState_t;

/**
 * @brief 任务通知合并动作
 */
typedef enum {
    eNoAction = 0,              ///< 仅置 pending
    eSetBits,                   ///< 按位或
    eIncrement,                 ///< 加一
    eSetValueWithOverwrite,     ///< 覆盖写入
    eSetValueWithoutOverwrite   ///< 仅当原值为 0 时写入
} eNotifyAction_t;

/* -------------------------------------------------------------------------- */
/* Task control block                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @struct cgrtos_task
 * @brief 任务控制块（TCB），栈内嵌于结构体尾部
 */
typedef struct cgrtos_task {
    uint64_t           *sp;              /* saved stack pointer (trap frame) */
    char                name[CGRTOS_TASK_NAME_MAX];
    task_id_t           id;              /* unique id; 0 = free / idle slot */
    uint8_t             prio;            /* current effective priority */
    uint8_t             base_prio;       /* priority before PI / DPCP boost */
#if CONFIG_USE_PREEMPT_THRESH
    uint8_t             preempt_thresh;  /* 抢占阈值：仅更高 prio 可抢占 */
#endif
    task_state_t        state;
    sched_policy_t      policy;
    task_func_t         entry_fn;        /* 用户入口（bootstrap 调用） */
    void               *entry_arg;
    block_reason_t      block_reason;
    tick_t              wake_tick;       /* delayed-list absolute wake time */
    tick_t              deadline;        /* EDF absolute deadline */
    tick_t              period;          /* EDF period (0 = sporadic) */
    tick_t              exec;            /* ticks spent running (stats) */
#if CONFIG_SCHED_STATS
    tick_t              ready_since;     /* 进入 READY 的 g_ticks */
    tick_t              max_sched_latency;
    tick_t              last_sched_latency;
    uint64_t            sched_latency_sum;
    uint32_t            sched_latency_samples;
#endif
    tick_t              vruntime;        /* CFS virtual runtime */
    tick_t              last_run;        /* last dispatch tick */
    tick_t              slice_remain;    /* RR/CFS remaining time slice */
    uint8_t             cpu_aff;         /* 0xFF = any core; else hard pin */
    uint8_t             run_cpu;         /* last / intended home core */
    void               *block_obj;       /* IPC object while blocked */
    event_flags_t       event_wait_mask;
    uint8_t             event_wait_all;
#if CONFIG_USE_TASK_NOTIFICATIONS
    uint32_t            notify_value;
    uint8_t             notify_pending;
#endif
    uint8_t             wake_ok;         /* 1 = signal wake, 0 = timeout */
    list_item_t         delayed_item;    /* sorted delayed list node */
    list_item_t         cfs_item;        /* CFS ready list by vruntime */
    list_item_t         edf_item;        /* EDF ready list by deadline */
    list_item_t         edf_rel_item;    /* EDF next-release wheel node */
    uint8_t             edf_on_wheel;    /* 1 if armed on release wheel */
    struct cgrtos_task *volatile next;   /* priority ready / wait-q link */
    struct cgrtos_task *volatile prev;
    uint64_t            stack[CONFIG_TASK_STACK_WORDS]; /* embedded stack */
} cgrtos_task_t;

/** @brief Idle / Tick 钩子函数类型 */
typedef void (*cgrtos_hook_fn_t)(void);
/** @brief 分配失败钩子 */
typedef void (*cgrtos_malloc_failed_hook_t)(unsigned long size);
/** @brief 断言失败钩子（在 halt 前调用；可为 NULL） */
typedef void (*cgrtos_assert_hook_t)(const char *file, int line);
/** @brief 栈溢出钩子（默认路径随后仍会 assert halt） */
typedef void (*cgrtos_stack_overflow_hook_t)(cgrtos_task_t *task);

/**
 * @struct cgrtos_runtime_stats_t
 * @brief 可查询的运行时统计快照（由 cgrtos_stats_get 填充）
 */
typedef struct {
    tick_t         uptime_ticks;                          /**< 系统运行 tick */
    uint32_t       tasks_alive;                           /**< 存活任务数 */
    uint32_t       context_switches;                      /**< 全局上下文切换 */
    uint32_t       context_switches_core[CONFIG_NUM_CORES];/**< 每核切换次数 */
    uint32_t       lb_migrate;                            /**< Push 迁移次数 */
    uint32_t       lb_steal;                              /**< Idle Steal 次数 */
    uint32_t       task_creates;                          /**< 累计创建次数 */
    uint32_t       task_deletes;                          /**< 累计删除次数 */
    uint32_t       stack_overflows;                       /**< 栈溢出检测次数 */
    uint32_t       asserts;                               /**< 断言失败次数 */
    unsigned long  free_heap;                             /**< 当前空闲堆 */
    unsigned long  min_free_heap;                         /**< 历史最小空闲堆 */
#if CONFIG_SCHED_STATS
    tick_t         max_sched_latency;                     /**< 全局最大调度延迟 */
    uint32_t       sched_latency_samples;                 /**< 延迟采样次数 */
#endif
} cgrtos_runtime_stats_t;

#if CONFIG_SCHED_STATS
/**
 * @brief 单任务调度统计
 */
typedef struct {
    tick_t   max_latency;
    tick_t   last_latency;
    uint64_t latency_sum;
    uint32_t latency_samples;
    tick_t   exec_ticks;
    uint32_t cpu_util_permille; /**< exec/uptime*1000；uptime=0 则为 0 */
} cgrtos_task_sched_stats_t;
#endif

/* -------------------------------------------------------------------------- */
/* IPC objects                                                                */
/* -------------------------------------------------------------------------- */

struct cgrtos_queue_set; /* forward */

/**
 * @struct cgrtos_sem_t
 * @brief 计数信号量
 */
typedef struct {
    int32_t             count;   /**< 当前计数 */
    int32_t             max;     /**< 最大计数 */
    spinlock_t          lock;    /**< 对象锁（与临界区配合） */
    cgrtos_task_t      *volatile wait_q; /**< 优先级等待队列头 */
    struct cgrtos_queue_set *qset; /**< 所属 QueueSet（可空） */
} cgrtos_sem_t;

/**
 * @struct cgrtos_mutex_t
 * @brief 递归互斥量（可选优先级继承字段）
 */
typedef struct {
    cgrtos_task_t      *owner;     /**< 当前持有者 */
    uint8_t             owner_prio;/**< 持有者拿锁时的优先级 */
    uint8_t             inherit;   /**< 1=优先级继承 PI（非 DPCP 时） */
    uint8_t             in_use;    /**< 池槽占用标志 */
#if CONFIG_USE_DPCP
    uint8_t             dpcp;      /**< 1=启用 DPCP 天花板 */
    uint8_t             ceiling_prio; /**< FP 优先级天花板 */
    tick_t              ceiling_rel;  /**< EDF 相对 deadline 天花板；0=不用 */
    uint8_t             saved_prio;   /**< 加锁前 prio */
    tick_t              saved_deadline; /**< 加锁前 deadline */
    uint8_t             ceiling_applied; /**< 是否已应用天花板 */
#endif
    uint32_t            recursive; /**< 递归加锁深度 */
    spinlock_t          lock;
    cgrtos_task_t      *volatile wait_q;
} cgrtos_mutex_t;

/**
 * @struct cgrtos_queue_t
 * @brief 定长消息队列
 */
typedef struct {
    void               *buf;       /**< 环形缓冲 */
    uint32_t            item_sz;   /**< 单元素字节数 */
    uint32_t            len;       /**< 容量（元素个数） */
    uint32_t            cnt;       /**< 当前消息数 */
    uint32_t            head;      /**< 写下标（入队） */
    uint32_t            tail;      /**< 读下标（出队） */
    uint8_t             storage_static; /**< 1=外部静态存储，不 free */
    spinlock_t          lock;
    cgrtos_task_t      *volatile send_wait_q; /**< 满队列等待发送者 */
    cgrtos_task_t      *volatile recv_wait_q; /**< 空队列等待接收者 */
    struct cgrtos_queue_set *qset; /**< 所属 QueueSet（可空） */
} cgrtos_queue_t;

/**
 * @struct cgrtos_stream_buffer_t
 * @brief 字节流缓冲（环形）；`is_message=1` 时为 MessageBuffer（每条消息带 2 字节长度前缀）
 */
typedef struct cgrtos_stream_buffer {
    uint8_t            *buf;           /**< 环形数据区（堆分配） */
    uint32_t            size;          /**< 容量（字节） */
    uint32_t            head;          /**< 写位置 */
    uint32_t            tail;          /**< 读位置 */
    uint32_t            avail;         /**< 可读字节数 */
    uint32_t            trigger;       /**< 流模式下唤醒接收方的最低可读水位 */
    uint8_t             is_message;    /**< 1=MessageBuffer（整消息原子收发） */
    uint8_t             in_use;        /**< 对象池占用标志 */
    cgrtos_task_t      *volatile send_wait_q; /**< 等空间的发送者 */
    cgrtos_task_t      *volatile recv_wait_q; /**< 等数据的接收者 */
    struct cgrtos_queue_set *qset;     /**< 所属 QueueSet（可空） */
} cgrtos_stream_buffer_t;

/**
 * @typedef cgrtos_message_buffer_t
 * @brief MessageBuffer：底层即 StreamBuffer，发送时写入 `[u16 len][payload]`，接收一次取一整条
 */
typedef cgrtos_stream_buffer_t cgrtos_message_buffer_t;

/**
 * @enum cgrtos_qset_type_t
 * @brief QueueSet 成员类型
 */
typedef enum {
    CGRTOS_QSET_QUEUE = 1, /**< 普通消息队列 */
    CGRTOS_QSET_SEM,       /**< 计数/二进制信号量 */
    CGRTOS_QSET_STREAM     /**< StreamBuffer / MessageBuffer */
} cgrtos_qset_type_t;

/**
 * @struct cgrtos_queue_set_t
 * @brief 在多个 Queue / Sem / Stream 上 select：任一成员就绪则返回其对象指针
 */
typedef struct cgrtos_queue_set {
    struct {
        cgrtos_qset_type_t type; /**< 成员类型 */
        void              *obj;  /**< 成员对象指针 */
    } members[CGRTOS_QUEUE_SET_LENGTH];
    uint32_t            n_members;
    uint8_t             ready[CGRTOS_QUEUE_SET_LENGTH];
    uint32_t            rq_head;
    uint32_t            rq_tail;
    uint32_t            rq_cnt;
    uint8_t             in_use;
    cgrtos_task_t      *volatile wait_q;
} cgrtos_queue_set_t;

/**
 * @struct cgrtos_timer
 * @brief 软件定时器控制块
 */
typedef struct cgrtos_timer {
    char                name[CGRTOS_TASK_NAME_MAX];
    timer_cb_t          cb;        /**< 到期回调 */
    void               *arg;       /**< 回调参数 */
    tick_t              period;    /**< 周期（tick） */
    tick_t              remain;    /**< 剩余/武装值 */
    uint8_t             active;    /**< 是否在轮上 */
    uint8_t             periodic;  /**< 1=周期，0=单次 */
    struct cgrtos_timer *next;     /**< 调试/链表预留 */
} cgrtos_timer_t;

/**
 * @struct cgrtos_event_group_t
 * @brief 事件标志组
 */
typedef struct {
    event_flags_t       flags;     /**< 当前置位位图 */
    uint8_t             in_use;
    spinlock_t          lock;
    cgrtos_task_t      *volatile wait_q;
} cgrtos_event_group_t;

/**
 * @struct timer_cmd_t
 * @brief 定时器 daemon 命令（ISR → 任务队列）
 */
typedef struct {
    cgrtos_timer_t     *timer;  /**< 关联定时器 */
    uint8_t             cmd;    /**< 命令码：EXPIRE/START/STOP/RESET/CHANGE */
    tick_t              period; /**< CHANGE 命令的新周期 */
} timer_cmd_t;

/**
 * @typedef cgrtos_irq_handler_t
 * @brief PLIC 外部中断处理函数类型
 * @param irq claim 得到的中断源号
 * @param arg 注册时传入的私有指针
 * @note 须短小非阻塞；仅当该源优先级 ≤ syscall_max 时方可调用 FromISR
 */
typedef void (*cgrtos_irq_handler_t)(uint32_t irq, void *arg);

/**
 * @defgroup cgrtos_fs RAM 文件系统
 * @brief 纯内存 POSIX 风格 FS：根为 `/`，数据在堆上，inode/fd 为静态池
 * @{
 */
/** @def CGRTOS_O_RDONLY 只读打开 */
#define CGRTOS_O_RDONLY   0x0001
/** @def CGRTOS_O_WRONLY 只写打开 */
#define CGRTOS_O_WRONLY   0x0002
/** @def CGRTOS_O_RDWR 读写打开 */
#define CGRTOS_O_RDWR     0x0003
/** @def CGRTOS_O_CREAT 不存在则创建文件 */
#define CGRTOS_O_CREAT    0x0100
/** @def CGRTOS_O_TRUNC 打开时截断为 0 */
#define CGRTOS_O_TRUNC    0x0200
/** @def CGRTOS_O_APPEND 写操作追加到文件末尾 */
#define CGRTOS_O_APPEND   0x0400

/** @def CGRTOS_S_IFREG 普通文件（`cgrtos_stat_t.mode`） */
#define CGRTOS_S_IFREG    0x8000
/** @def CGRTOS_S_IFDIR 目录 */
#define CGRTOS_S_IFDIR    0x4000

/** @brief `cgrtos_fs_stat` 输出：类型与大小 */
typedef struct {
    uint32_t mode; /**< `CGRTOS_S_IFREG` 或 `CGRTOS_S_IFDIR` */
    uint32_t size; /**< 文件字节数；目录为 dentry 打包长度 */
} cgrtos_stat_t;

/**
 * @brief `cgrtos_fs_statfs` / VFS df 输出：容量与 inode 池用量
 */
typedef struct {
    uint32_t blocks_total; /**< 逻辑块总数（按 512B 估算） */
    uint32_t blocks_used;  /**< 已用逻辑块 */
    uint32_t inodes_total; /**< inode 池容量 */
    uint32_t inodes_used;  /**< 已占用 inode（含根） */
    uint32_t name_max;     /**< 单组件名最大长度 */
    uint32_t max_file;     /**< 单文件最大字节 */
} cgrtos_statfs_t;

/** @brief `cgrtos_fs_readdir` 输出的目录项 */
typedef struct {
    char     name[CGRTOS_FS_MAX_NAME]; /**< 短文件名（不含路径） */
    uint32_t mode;                     /**< 同 `cgrtos_stat_t.mode` */
} cgrtos_dirent_t;

/** @brief 目录迭代句柄（不透明；由 opendir/closedir 管理） */
typedef struct cgrtos_dir cgrtos_dir_t;
/** @} */

/* -------------------------------------------------------------------------- */
/* Global kernel state                                                        */
/* -------------------------------------------------------------------------- */

/** @brief 任务 TCB 池 */
extern cgrtos_task_t    g_tasks[CONFIG_MAX_TASKS];
/** @brief 当前存活任务数（不含纯 idle 槽语义以实现为准） */
extern uint32_t         g_task_count;
/** @brief 每核当前运行任务指针 */
extern cgrtos_task_t   *g_current[CONFIG_NUM_CORES];
/** @brief 每核 Idle 任务 TCB */
extern cgrtos_task_t    g_idle[CONFIG_NUM_CORES];
/** @brief 全局系统节拍（通常仅 hart0 递增） */
extern tick_t           g_ticks;
/** @brief 全局上下文切换次数 */
extern uint32_t         g_cs_count;
/** @brief 每核上下文切换次数 */
extern uint32_t         g_cs_count_core[CONFIG_NUM_CORES];
/** @brief Push 负载迁移次数 */
extern uint32_t         g_lb_migrate_count;
/** @brief Idle Steal 次数 */
extern uint32_t         g_lb_steal_count;
/** @brief 调度器已启动标志（secondary 等待此位） */
extern uint8_t          g_sched_run;
/** @brief 全局内核自旋锁（临界区） */
extern spinlock_t       g_klock;
/** @brief 每核请求在 trap 出口重新调度 */
extern volatile uint8_t g_yield_pending[CONFIG_NUM_CORES];
/** @brief hart0 请求次核执行本地时间片记账 */
extern volatile uint8_t g_remote_tick[CONFIG_NUM_CORES];
/**
 * @brief 次核在线位图：bit N（N≥1）表示 hart N 已进入调度器
 * @note 查询请用 CGRTOS_CORE_ONLINE(c)；非零即「至少一个次核在线」（兼容旧测试）
 */
extern volatile uint32_t g_secondary_online;
/**
 * @brief 各 hart 启动阶段面包屑（.data；startup.S 写入，kick 轮询）
 * @note 下标 0 不用；次核到达 4 表示已通过 g_boot_sync 同步
 */
extern volatile uint32_t g_hart_stage[CONFIG_MAX_CORES];

/**
 * @def CGRTOS_CORE_ONLINE
 * @brief 判断逻辑核是否可调度（hart0 恒为真；次核看 g_secondary_online 位）
 */
#define CGRTOS_CORE_ONLINE(c) \
    (((uint8_t)(c) == 0U) || \
     (((uint8_t)(c) < (uint8_t)CONFIG_NUM_CORES) && \
      ((g_secondary_online & (1U << (uint8_t)(c))) != 0U)))
/** @brief SMP 启动同步魔数（startup.S 轮询） */
extern volatile uint32_t g_boot_sync;

/* -------------------------------------------------------------------------- */
/* Architecture / startup                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 保存 *cur 栈指针并切换到 nxt（兼容入口）
 * @details 通常由 ecall yield / trap 路径间接使用；直接调用较少见。
 * @param[in,out] cur 指向当前任务 SP 保存槽的指针；不可为 NULL
 * @param[in]     nxt 下一任务栈指针
 * @return 无
 * @retval 无
 * @note 实现位于架构汇编/ trampoline
 * @warning 禁止在持有不合理锁状态时调用；应用层请用 cgrtos_task_yield
 * @attention ❌ 非普通 ISR API；✅ 立即上下文切换
 */
extern void context_switch(uint64_t **cur, uint64_t *nxt);
/**
 * @brief 装载首任务 trap 帧并 mret 进入任务上下文
 * @details 由 cgrtos_start / 次核启动路径调用，不返回到调用者。
 * @param[in] sp 首个任务栈指针（trap 帧顶）
 * @return 无（不返回）
 * @retval 无
 * @note 调用前须已选好 g_current
 * @warning 返回后代码不可达
 * @attention ❌ ISR；✅ 立即切换进入任务
 */
extern void start_first_task(uint64_t *sp);
/**
 * @brief 处理软件中断（MSIP / IPI）
 * @details
 * 1. cgrtos_isr_enter 标记 ISR 上下文。
 * 2. 清本核 MSIP；处理远程 tick 与 yield 挂起。
 * 3. cgrtos_isr_exit 退出 ISR 上下文。
 * @param[in] f 陷阱栈帧指针；实现可未使用
 * @return 无
 * @note 由 trap 向量调用；实现见 arch/riscv/ipic.c
 * @warning 禁止从任务上下文直接调用
 * @attention ✅ ISR；❌ 不阻塞
 */
extern void riscv_handle_ipi(uint64_t *f);
/**
 * @brief 处理定时器中断（MTIP）
 * @details
 * 1. cgrtos_isr_enter 标记 ISR 上下文。
 * 2. 重载 mtimecmp 并调用 cgrtos_tick_handler。
 * 3. cgrtos_isr_exit 退出 ISR 上下文。
 * @param[in] f 陷阱栈帧指针；实现可未使用
 * @return 无
 * @note 由 trap 向量调用；实现见 arch/riscv/clint.c
 * @warning 禁止从任务上下文直接调用
 * @attention ✅ ISR；❌ 不阻塞
 */
extern void riscv_handle_timer(uint64_t *f);
/**
 * @brief 处理外部中断（MEIP / PLIC）
 * @details
 * 1. claim → 抬 threshold → 可选嵌套窗口 → cgrtos_irq_dispatch → complete。
 * 2. 底层直调 plic_hw_*，不经 hal_irqc_*。
 * @param[in] f 陷阱栈帧指针；实现可未使用
 * @return 无
 * @note 由 trap 向量调用；实现见 arch/riscv/plic.c
 * @warning 禁止从任务上下文直接调用
 * @attention ✅ ISR；❌ 不阻塞
 */
extern void riscv_handle_external(uint64_t *f);
/**
 * @brief 处理同步异常并输出早期诊断信息
 * @details
 * 1. 经 drv_uart_early_puts 粗打 cause / epc 十六进制。
 * 2. 不依赖 HAL 或 printf 格式化库。
 * @param[in] f     陷阱栈帧指针；实现可未使用
 * @param[in] cause mcause 异常码
 * @param[in] epc   mepc 异常 PC
 * @return 无
 * @note 由 trap 向量在同步异常路径调用；实现见 arch/riscv/plic.c
 * @warning 输出后轮询阻塞；通常随后挂起
 * @attention ✅ ISR；✅ 阻塞（轮询 TX FIFO）
 */
extern void riscv_handle_exception(uint64_t *f, uint64_t cause, uint64_t epc);

/**
 * @brief 致命挂起循环（反复 WFI）
 * @details 断言失败/不可恢复错误后进入，不再返回。
 * @return 无（不返回）
 * @retval 无
 * @note 可由钩子在 halt 前做最后日志
 * @warning 进入后系统停止调度
 * @attention ✅ 可从 ISR/任务调用；❌ 不“阻塞等待事件”，永久挂起
 */
void _deadloop(void);
/**
 * @brief 应用入口（hart0）
 * @details 由 startup 在 hart0 跳入；典型路径：cgrtos_init → 创建任务 → cgrtos_start。
 * @param[in] hartid 核号（应为 0）
 * @param[in] fdt    设备树指针（可忽略）
 * @param[in] end    链接脚本堆/镜像末等（可忽略）
 * @return 通常不返回；若返回则实现定义
 * @retval 0 约定成功（若可达）
 * @note 由各 APP（demo/test/cli）分别实现
 * @warning 勿在次核调用
 * @attention ❌ ISR；✅ 随后启动调度会切换
 */
int main(int hartid, void *fdt, void *end);
/**
 * @brief 从核 C 入口
 * @details 等待 SMP 同步后进入 cgrtos_start_secondary。
 * @param[in] hartid 核号（通常 ≥1）
 * @return 无（不返回）
 * @retval 无
 * @note 由各 APP 实现或弱符号提供
 * @warning 勿在 hart0 调用
 * @attention ❌ ISR；✅ 进入调度后持续可能切换
 */
void secondary_main(int hartid);

/* -------------------------------------------------------------------------- */
/* Kernel API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化内核全局状态、HAL 板级外设并唤醒次核
 * @details 清零 TCB/临界区/钩子，调用调度器与 hal_board_init，SMP kick 次核并打印横幅。
 *          不启动调度器（须再调 cgrtos_start）。
 * @return 无
 * @retval 无
 * @note 仅 hart0 在创建用户任务前调用一次
 * @warning 重复调用可能导致设备二次 init / 状态错乱
 * @attention ❌ ISR；❌ 本函数不切换用户任务（次核 kick 发 IPI）
 */
void cgrtos_init(void);

/**
 * @brief 启动调度器并切入首个任务（hart0，不返回）
 * @details 初始化 idle/定时服务，置 g_sched_run，选择首个 READY 任务并 start_first_task。
 * @return 无（不返回）
 * @retval 无
 * @note 调用前应已创建至少一个应用任务（否则跑 idle）
 * @warning 返回后的代码不可达
 * @attention ❌ ISR；✅ 立即开始上下文切换
 */
void cgrtos_start(void);

/**
 * @brief 次核调度入口：等待 g_sched_run 后运行本核 idle
 * @details 发布 online 位后进入 idle 循环，响应 IPI/远程 tick。
 * @param[in] hartid 本核逻辑编号（1..CONFIG_NUM_CORES-1）
 * @return 无（不返回）
 * @retval 无
 * @note 由 secondary_main 调用
 * @warning 勿在 hart0 调用
 * @attention ❌ ISR；✅ 持续可能发生切换
 */
void cgrtos_start_secondary(int hartid);

/**
 * @brief 初始化每核 idle 任务 TCB 与栈帧
 * @details 填充 g_idle[]、绑定 cgrtos_idle_task_entry，并设置 g_current 初值。
 * @return 无
 * @retval 无
 * @note 由 cgrtos_start 内部调用；也可在启动路径显式调用
 * @warning 调度运行中重复调用不安全
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_init_idle_tasks(void);

/**
 * @brief 创建任务并加入就绪队列
 * @details 从 TCB 池分配槽，初始化栈（经 bootstrap）、策略与亲和性，加入就绪结构；
 *          SMP 下可能向目标核发 IPI。
 * @param[in] name   任务名；NULL 时使用默认名；"tmp" 可抑制创建日志
 * @param[in] fn     入口函数；不可为 NULL
 * @param[in] arg    传给入口的参数；可为 NULL
 * @param[in] prio   优先级 0..CONFIG_MAX_PRIORITY（越界截断）
 * @param[in] policy 调度策略；CONFIG_USE_EDF=0 时 SCHED_EDF 失败
 * @return 新任务 ID
 * @retval >0           成功
 * @retval (task_id_t)-1 参数非法 / 池满 / 策略禁用
 * @note 任务入口返回后经 bootstrap 调用 cgrtos_task_exit
 * @warning 禁止应用直接改写返回 ID 对应 TCB 字段
 * @attention ❌ ISR；❌ 通常不阻塞调用者（可能 IPI 触发他核调度）
 */
task_id_t cgrtos_task_create(const char *name, task_func_t fn, void *arg,
                             uint8_t prio, sched_policy_t policy);

/**
 * @brief 删除指定任务并回收资源
 * @details 释放其持有互斥量、摘就绪/等待队列、置 DELETED；若仍在他核运行则 IPI 后延迟 reclaim。
 * @param[in] id 任务 ID；0 无效
 * @return 操作结果
 * @retval pdPASS 成功标记删除
 * @retval pdFAIL 任务不存在或试图删除 idle
 * @note 自删除会 yield 切走
 * @warning 删除仍持锁任务依赖 force_release；与等待者存在竞态须由临界区保护（内部已处理）
 * @attention ❌ ISR；✅ 可能触发调度（自删/同核 RUNNING）
 */
int cgrtos_task_delete(task_id_t id);

/**
 * @brief 挂起任务使其退出调度
 * @details 从就绪队列移除并置 SUSPENDED；对 RUNNING 目标会请求重调度。
 * @param[in] id 任务 ID
 * @return 操作结果
 * @retval pdPASS 成功
 * @retval pdFAIL 无效 ID
 * @note 可用 resume 恢复
 * @warning 挂起持有互斥量的任务可能导致优先级反转/死锁
 * @attention ❌ ISR；✅ 可能引起切换
 */
int cgrtos_task_suspend(task_id_t id);

/**
 * @brief 恢复被挂起的任务
 * @details 置 READY 并入队；必要时 IPI。
 * @param[in] id 任务 ID
 * @return 操作结果
 * @retval pdPASS 成功
 * @retval pdFAIL 无效或不在挂起态（以实现为准）
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能引起切换
 */
int cgrtos_task_resume(task_id_t id);

/**
 * @brief 设置任务当前优先级与 base_prio
 * @details READY 时先出队再改优先级再入队；可能触发 PI/阈值字段调整。
 * @param[in] id   任务 ID
 * @param[in] prio 新优先级 0..CONFIG_MAX_PRIORITY
 * @return 操作结果
 * @retval pdPASS 成功
 * @retval pdFAIL 越界或任务不存在
 * @note 与 PI/DPCP 共存时 base_prio 更新，有效 prio 可能仍被抬升
 * @warning 降低持锁任务优先级可能加剧优先级反转窗口
 * @attention ❌ ISR；✅ 可能 yield
 */
int cgrtos_task_set_priority(task_id_t id, uint8_t prio);

#if CONFIG_USE_PREEMPT_THRESH
/**
 * @brief 设置抢占阈值
 * @details 仅当候选任务 prio > thresh 时才允许抢占当前（FP/Hybrid-RT 粘性路径）。
 * @param[in] id     任务 ID
 * @param[in] thresh 阈值；须 >= base_prio 且 <= CONFIG_MAX_PRIORITY
 * @return 操作结果
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或任务不存在
 * @note 默认 create 时 thresh=prio（经典抢占）
 * @warning 阈值过高会推迟高优先级响应，仅用于短临界段降抖动
 * @attention ❌ ISR；✅ 可能 yield
 */
int cgrtos_task_set_preempt_threshold(task_id_t id, uint8_t thresh);

/**
 * @brief 读取任务抢占阈值
 * @details 临界区内读取 preempt_thresh 字段。
 * @param[in] id 任务 ID
 * @return 阈值数值；无效任务返回 0
 * @retval 0..CONFIG_MAX_PRIORITY 有效阈值
 * @retval 0 亦可能表示无效任务，调用方应先校验 id
 * @note 无
 * @warning 无
 * @attention ❌ ISR（持 g_klock）；❌ 不阻塞
 */
uint8_t cgrtos_task_get_preempt_threshold(task_id_t id);
#endif

/**
 * @brief 当前任务正常退出（TERMINATED→DELETED）
 * @details 置 TERMINATED 后调用 delete(self)；之后死循环 yield。bootstrap 在入口返回时自动调用。
 * @return 无（永不返回）
 * @retval 无
 * @note idle 禁止退出
 * @warning 在持有互斥量时退出依赖 delete 的 force_release
 * @attention ❌ ISR；✅ 必定引起调度
 */
void cgrtos_task_exit(void);

/**
 * @brief 设置任务 CPU 亲和性
 * @details 0xFF 表示可迁移；否则硬绑定到指定 hart（越界/次核未上线时回退策略见实现）。
 * @param[in] id  任务 ID
 * @param[in] cpu 0xFF=任意；或 0..CONFIG_NUM_CORES-1
 * @return 操作结果
 * @retval pdPASS 成功
 * @retval pdFAIL 无效任务
 * @note 可能伴随迁移 IPI
 * @warning 硬绑满核可能导致过载无法迁移
 * @attention ❌ ISR；✅ 可能引起切换
 */
int cgrtos_task_set_affinity(task_id_t id, uint8_t cpu);

/**
 * @brief 设置 EDF 周期并武装释放时间轮
 * @details period>0 时更新 deadline=now+period 并 cgrtos_sched_edf_arm。
 * @param[in] id     任务 ID
 * @param[in] period 周期 tick；0 表示取消周期/卸轮
 * @return 操作结果
 * @retval pdPASS 成功
 * @retval pdFAIL 任务不存在或 CONFIG_USE_EDF=0
 * @note 仅对 SCHED_EDF 有意义
 * @warning 过短周期可能导致持续过载与错过 deadline
 * @attention ❌ ISR；❌ 通常不阻塞（arm 持 ready_lock）
 */
int cgrtos_task_set_period(task_id_t id, tick_t period);

/**
 * @brief 设置 EDF 绝对截止时间
 * @details 更新 deadline；若任务 READY 则按新键重入就绪结构；period>0 时同步释放轮。
 * @param[in] id       任务 ID
 * @param[in] deadline 绝对 tick（相对 g_ticks 的时间点）
 * @return 操作结果
 * @retval pdPASS 成功
 * @retval pdFAIL 任务不存在
 * @note 无
 * @warning deadline 已过期时仍可能被选中运行（取决于 pick 规则）
 * @attention ❌ ISR；❌ 通常不阻塞
 */
int cgrtos_task_set_deadline(task_id_t id, tick_t deadline);

/**
 * @brief 由任务 ID 获取 TCB 指针
 * @details 线性查找 g_tasks；供内核/高级同步使用。
 * @param[in] id 任务 ID
 * @return TCB 指针
 * @retval 非 NULL 找到
 * @retval NULL    未找到
 * @note 应用应视 TCB 为只读句柄，禁止直接改字段
 * @warning 返回指针在任务删除后可能失效（UAF）
 * @attention ✅ 可在 ISR 调用（无锁查找，存在撕裂风险）；❌ 不阻塞
 */
cgrtos_task_t *cgrtos_task_get_handle(task_id_t id);

/**
 * @brief 查询任务对外可见状态
 * @details 映射内部 task_state_t 到 eTaskState_t。
 * @param[in] id 任务 ID
 * @return 状态枚举
 * @retval eReady/eRunning/eBlocked/eSuspended/eDeleted/eTerminated 对应状态
 * @retval eInvalid 无效 ID
 * @note 无
 * @warning 无锁快照，SMP 下可能略有滞后
 * @attention ✅ ISR；❌ 不阻塞
 */
eTaskState_t cgrtos_task_get_state(task_id_t id);

/**
 * @brief 查询任务最近运行/归属核号
 * @details 返回 TCB 的 run_cpu 字段。
 * @param[in] id 任务 ID
 * @return 核号或 0xFF
 * @retval 0..CONFIG_NUM_CORES-1 有效
 * @retval 0xFF 无效任务
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
uint8_t cgrtos_task_get_run_cpu(task_id_t id);

/**
 * @brief 查询栈高水位（剩余未使用栈字节估测）
 * @details 自栈底扫描填充模式字（0xA5 或 0）累计空闲字再换算字节。
 * @param[in] id 任务 ID
 * @return 剩余空闲栈字节数
 * @retval >0 估测值
 * @retval 0  无效任务或已几乎用尽
 * @note 需 CONFIG_CHECK_STACK_OVERFLOW 填栈才准确
 * @warning 非精确值，仅调试用
 * @attention ✅ 任务上下文更安全；❌ 不阻塞
 */
uint32_t cgrtos_task_get_stack_high_water_mark(task_id_t id);

/**
 * @brief 当前任务自愿让出 CPU
 * @details 置 force_yield 并 ecall 进入 trap 切换路径。
 * @return 无
 * @retval 无
 * @note 调度挂起时为空操作
 * @warning 无
 * @attention ❌ ISR（ISR 请用 yield_from_isr）；✅ 引起切换
 */
void cgrtos_task_yield(void);

/**
 * @brief 相对延时若干系统 tick
 * @details ticks==0 仅 yield；否则以 BLOCK_DELAY 入延迟链后 yield，到期由 tick 唤醒。
 * @param[in] ticks 延时 tick 数；0=仅让出；portMAX_DELAY 语义不适用于本 API（用大值近似）
 * @return 无
 * @retval 无
 * @note ISR 中调用直接返回
 * @warning 关调度时 delay 不会真正阻塞切换
 * @attention ❌ ISR；✅ ticks>0 时阻塞并切换
 */
void cgrtos_delay(tick_t ticks);

/**
 * @brief 阻塞到绝对系统 tick
 * @details wake<=now 立即返回；否则 block_until 后 yield。
 * @param[in] wake 绝对 g_ticks 时刻
 * @return 无
 * @retval 无
 * @note 仅任务上下文
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
void cgrtos_delay_until_tick(tick_t wake);

/**
 * @brief 相对延时（毫秒）
 * @details 换算为 tick 后调用 cgrtos_delay；精度受 tick 粒度限制。
 * @param[in] ms 毫秒；0 等价 yield
 * @return 无
 * @retval 无
 * @note 仅任务上下文
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
void cgrtos_delay_ms(uint32_t ms);

/**
 * @brief 相对延时（微秒）
 * @details 短延时 mtime 忙等；较长则 tick 粗阻塞 + mtime 补齐。
 * @param[in] us 微秒；0 无操作
 * @return 无
 * @retval 无
 * @note ISR 中无操作
 * @warning 长 us 忙等会拉长关调度窗口外的占用
 * @attention ❌ ISR；✅ 可能阻塞（长延时）
 */
void cgrtos_delay_us(uint32_t us);

/**
 * @brief 绝对周期延时（vTaskDelayUntil 语义）
 * @details *prev_wake 更新为下一唤醒点；若已过期则不阻塞仅推进。
 * @param[inout] prev_wake 上次唤醒 tick；不可为 NULL
 * @param[in]    increment 周期 tick；0 则忽略
 * @return 无
 * @retval 无
 * @note 用于周期任务对齐
 * @warning prev_wake 未正确维护会导致相位漂移
 * @attention ❌ ISR；✅ 可能阻塞
 */
void cgrtos_delay_until(tick_t *prev_wake, tick_t increment);

#if CONFIG_USE_TASK_NOTIFICATIONS
/**
 * @brief 向目标任务发送通知并可能唤醒
 * @details 按 action 合并 notify_value；若目标在 BLOCK_NOTIFY 则 unblock。
 * @param[in] task   目标 TCB；不可为 NULL
 * @param[in] value  通知载荷
 * @param[in] action 合并动作（eSetBits/eIncrement/...）
 * @return 写入前的旧 notify_value；task 无效时为 0
 * @retval 任意 uint32_t 旧值
 * @note 无
 * @warning 与 ISR 通知并发时依赖临界区
 * @attention ❌ 阻塞型等待在 notify_wait；本 API ❌ ISR 请用 from_isr；✅ 可能唤醒切换
 */
uint32_t cgrtos_task_notify(cgrtos_task_t *task, uint32_t value,
                            eNotifyAction_t action);

/**
 * @brief 当前任务等待通知
 * @details 可按掩码在进入/退出时清位；超时返回 0。
 * @param[in]  clear_on_entry 进入等待前清除的位
 * @param[in]  clear_on_exit  成功返回前清除的位
 * @param[out] value          可选；非 NULL 时输出当前 notify_value
 * @param[in]  timeout        0=尝试；portMAX_DELAY=永久；否则相对 tick
 * @return 非 0 表示成功取得通知
 * @retval 非0 成功
 * @retval 0   超时或失败
 * @note 无
 * @warning 通知值本为 0 时与超时返回 0 可能混淆，请结合业务区分
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
uint32_t cgrtos_task_notify_wait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                                 uint32_t *value, tick_t timeout);

/**
 * @brief ISR 安全任务通知
 * @details 临界区内更新通知；可 unblock BLOCK_NOTIFY 等待者。
 * @param[in]  task   目标 TCB
 * @param[in]  value  载荷
 * @param[in]  action 动作
 * @param[out] woken  可选；唤醒更高优先级时置 pdTRUE；NULL 则自动 yield_from_isr
 * @return 更新前的 notify_value；task 为 NULL 返回 0
 * @retval 任意 uint32_t
 * @note 须在允许的中断优先级内调用（见 IRQ 分组）
 * @warning 忽略 woken 且未自动 yield 可能导致延迟调度
 * @attention ✅ ISR；❌ 不阻塞调用 ISR
 */
uint32_t cgrtos_task_notify_from_isr(cgrtos_task_t *task, uint32_t value,
                                     eNotifyAction_t action, BaseType_t *woken);
#endif

/**
 * @brief 创建计数信号量（池分配）
 * @details 在 g_sems[] 找空槽，初始化 count/max/wait_q。
 * @param[in] init 初值；须 0..max
 * @param[in] max  上限；须 >0
 * @return 信号量指针
 * @retval 非 NULL 成功
 * @retval NULL    参数非法或池满
 * @note 对象在静态池中，用户不得 free 指针
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_sem_t *cgrtos_sem_create(int32_t init, int32_t max);

/**
 * @brief 创建二进制信号量
 * @details 等价 create(0,1) 或满量程二值实现。
 * @return 信号量指针
 * @retval 非 NULL 成功
 * @retval NULL    池满
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_sem_t *cgrtos_sem_create_binary(void);

/**
 * @brief 在调用者存储上静态创建信号量
 * @details 不占用全局池；调用方保证 sem 生命周期。
 * @param[out] sem  用户提供的对象存储；不可为 NULL
 * @param[in]  init 初值
 * @param[in]  max  上限
 * @return sem 或 NULL
 * @retval sem  成功
 * @retval NULL 参数非法
 * @note 无
 * @warning 勿对池对象与静态对象混用 delete 语义错误
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_sem_t *cgrtos_sem_create_static(cgrtos_sem_t *sem, int32_t init, int32_t max);

/**
 * @brief 获取信号量（P / take）
 * @details count>0 则递减返回；否则按 timeout 阻塞等待 give/超时。
 * @param[in] sem     信号量；不可为 NULL
 * @param[in] timeout 0=非阻塞；portMAX_DELAY=永久；否则相对 tick
 * @return 结果码
 * @retval pdPASS     取到令牌
 * @retval errPARAM   sem 为空
 * @retval errISR     在 ISR 中调用阻塞路径（CONFIG_ISR_API_GUARD）
 * @retval errTIMEOUT 超时或非阻塞未取到
 * @note 等待队列按优先级排序
 * @warning 与 mutex 不同，无所有权，错误配对 give/take 会导致计数错乱
 * @attention ❌ 阻塞路径禁止 ISR；✅ timeout>0 且无令牌时阻塞并切换
 */
int cgrtos_sem_take(cgrtos_sem_t *sem, tick_t timeout);

/**
 * @brief 释放信号量（V / give）
 * @details 有等待者则唤醒最高优先级；否则 count++（不超过 max）。
 * @param[in] sem 信号量
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数错误
 * @note 可能挂接 QueueSet poke
 * @warning 无
 * @attention ❌ 建议任务上下文；✅ 唤醒时可能切换
 */
int cgrtos_sem_give(cgrtos_sem_t *sem);

/**
 * @brief ISR 中释放信号量
 * @details 不阻塞；可 unblock 等待者并置 woken / 自动 yield_from_isr。
 * @param[in]  sem   信号量
 * @param[out] woken 可选；见实现约定
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数错误
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_sem_give_from_isr(cgrtos_sem_t *sem, BaseType_t *woken);

/**
 * @brief ISR 中非阻塞获取信号量
 * @details 仅当 count>0 时递减；从不等待。
 * @param[in] sem 信号量
 * @return 结果码
 * @retval pdPASS 取到
 * @retval pdFAIL 无令牌或参数错误
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_sem_take_from_isr(cgrtos_sem_t *sem);

/**
 * @brief 删除信号量并唤醒等待者（失败唤醒）
 * @details 等待者 wake_ok=0；池对象标记空闲。
 * @param[in] sem 信号量
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数错误
 * @note 无
 * @warning 删除后禁止再使用指针
 * @attention ❌ ISR；✅ 可能切换
 */
int cgrtos_sem_delete(cgrtos_sem_t *sem);

/**
 * @brief 创建互斥量（默认启用优先级继承）
 * @details 池分配；inherit=1。
 * @return 互斥量指针
 * @retval 非 NULL 成功
 * @retval NULL    池满
 * @note 支持递归加锁（上限 CONFIG_MUTEX_MAX_RECURSIVE）
 * @warning 勿在 ISR 中 lock
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_mutex_t *cgrtos_mutex_create(void);

#if CONFIG_USE_DPCP
/**
 * @brief 创建启用 DPCP 天花板的互斥量
 * @details inherit=0，写入 ceiling_prio / ceiling_rel。
 * @param[in] ceiling_prio FP 优先级天花板 0..CONFIG_MAX_PRIORITY
 * @param[in] ceiling_rel  EDF 相对 deadline 天花板；0=仅用优先级天花板
 * @return 互斥量指针
 * @retval 非 NULL 成功
 * @retval NULL    参数非法或池满
 * @note 与动态 PI 互斥
 * @warning 天花板过低无法抑制反转；过高影响其他任务
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_mutex_t *cgrtos_mutex_create_dpcp(uint8_t ceiling_prio, tick_t ceiling_rel);

/**
 * @brief 更新互斥量天花板（须无 owner）
 * @details 空闲时写入 dpcp 参数。
 * @param[in] mutex        互斥量
 * @param[in] ceiling_prio 新 FP 天花板
 * @param[in] ceiling_rel  新 EDF 相对天花板
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或仍有 owner
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_mutex_set_ceiling(cgrtos_mutex_t *mutex, uint8_t ceiling_prio,
                             tick_t ceiling_rel);
#endif

/**
 * @brief 静态创建互斥量
 * @details 使用调用者存储，默认 inherit。
 * @param[out] mutex 用户存储；不可为 NULL
 * @return mutex 或 NULL
 * @retval mutex 成功
 * @retval NULL  参数非法
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_mutex_t *cgrtos_mutex_create_static(cgrtos_mutex_t *mutex);

/**
 * @brief 互斥量加锁（可递归）
 * @details 空闲则占用；同 owner 递推；否则 PI/DPCP 后阻塞等待。可选死锁环检测。
 * @param[in] mutex   互斥量
 * @param[in] timeout 0=尝试；portMAX_DELAY=永久；否则相对 tick
 * @return 结果码
 * @retval pdPASS       加锁成功
 * @retval errPARAM     参数非法
 * @retval errISR       ISR 中调用
 * @retval errTIMEOUT   超时/非阻塞失败
 * @retval errDEADLOCK  检测到等待环（CONFIG_DETECT_DEADLOCK）
 * @retval errOVERFLOW  递归超过上限
 * @note 须与 unlock 成对
 * @warning 错误的锁顺序仍可能导致未被检测的死锁；持锁期间勿长时间阻塞其他资源
 * @attention ❌ ISR；✅ 可能阻塞并切换；可能触发优先级继承
 */
int cgrtos_mutex_lock(cgrtos_mutex_t *mutex, tick_t timeout);

/**
 * @brief 互斥量解锁
 * @details 递归则减层；否则恢复 PI/DPCP 并可能 handoff 给最高等待者。
 * @param[in] mutex 互斥量
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 非 owner 或参数非法
 * @note 无
 * @warning 非持有者 unlock 失败
 * @attention ❌ ISR；✅ 可能切换
 */
int cgrtos_mutex_unlock(cgrtos_mutex_t *mutex);

/**
 * @brief 删除互斥量
 * @details 唤醒等待者（失败）；回收池槽。
 * @param[in] mutex 互斥量
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 失败
 * @note 无
 * @warning 仍有 owner 时行为以实现为准，应先确保解锁
 * @attention ❌ ISR；✅ 可能切换
 */
int cgrtos_mutex_delete(cgrtos_mutex_t *mutex);
/**
 * @brief 查询互斥量递归额外层数
 * @details 临界区内读取 recursive；总持有次数 = 返回值 + 1（当 owner 非空）；无人持有时为 0。
 * @param[in] mutex 互斥量；可为 NULL（返回 0）
 * @return 递归层数（不含首次加锁）
 * @retval 0 owner 为空或参数非法
 * @retval >0 当前递归深度（不含 base lock）
 * @note 只读查询，不改变锁状态
 * @warning 与 lock/unlock 并发时数值为快照
 * @attention ❌ ISR（临界区）；❌ 不阻塞
 */
uint32_t cgrtos_mutex_get_recursive_count(cgrtos_mutex_t *mutex);
/**
 * @brief 查询互斥量当前持有者
 * @details 临界区内快照 owner 指针并返回。
 * @param[in] mutex 互斥量；可为 NULL
 * @return 持有者 TCB 指针
 * @retval 非 NULL 当前 owner
 * @retval NULL    无人持有或参数非法
 * @note 只读查询
 * @warning 与 lock/unlock 并发时指针为快照
 * @attention ❌ ISR（临界区）；❌ 不阻塞
 */
cgrtos_task_t *cgrtos_mutex_get_holder(cgrtos_mutex_t *mutex);
/**
 * @brief 强制释放指定任务持有的全部互斥量（任务删除安全）
 * @details 扫描全局互斥量池：对 owner==task 的对象恢复 PI/DPCP、清零 owner/recursive，并 handoff 给最高等待者或标记空闲。
 * @param[in] task 即将删除或已标记删除的任务；NULL 则直接返回
 * @return 无
 * @retval 无
 * @note 防止持锁任务被删导致等待者永久阻塞
 * @warning 须在任务删除路径调用；可能改变其它任务优先级
 * @attention ❌ ISR；✅ 可能切换
 */
void cgrtos_mutex_force_release_owned(cgrtos_task_t *task);

/**
 * @brief 动态创建消息队列
 * @details 在 g_qs[] 池分配槽位；堆上分配 len×item_sz 环形缓冲。
 * @param[in] len     队列容量（元素个数）；须 > 0
 * @param[in] item_sz 每个元素字节数；须 > 0
 * @return 队列指针
 * @retval 非 NULL 成功
 * @retval NULL    参数非法、池满或堆不足
 * @note delete 时会 free 动态缓冲
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_queue_t *cgrtos_queue_create(uint32_t len, uint32_t item_sz);
/**
 * @brief 在调用者静态存储上初始化消息队列
 * @details 绑定 storage 为环形缓冲，标记 storage_static=1；delete 时不 free 缓冲。
 * @param[out] q        用户提供的队列结构体；不可为 NULL
 * @param[in]  storage  环形缓冲，至少 len×item_sz 字节；不可为 NULL
 * @param[in]  len      元素个数；须 > 0
 * @param[in]  item_sz  元素大小；须 > 0
 * @return 队列指针
 * @retval q     成功
 * @retval NULL  参数非法
 * @note 不占用全局池计数
 * @warning storage 生命周期须覆盖队列使用期
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_queue_t *cgrtos_queue_create_static(cgrtos_queue_t *q, void *storage,
                                           uint32_t len, uint32_t item_sz);
/**
 * @brief 向队列发送一条消息
 * @details 队列未满则写入并可能唤醒 recv 等待者；满时按 timeout 挂入 send_wait_q 并 yield 重试。
 * @param[in] q       队列；不可为 NULL
 * @param[in] data    消息数据（item_sz 字节）；不可为 NULL
 * @param[in] timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 结果码
 * @retval pdPASS         成功
 * @retval errPARAM       参数非法
 * @retval errISR         ISR 中调用阻塞路径
 * @retval errQUEUE_FULL  非阻塞时队列满
 * @retval pdFAIL         超时或被 delete 唤醒
 * @note 唤醒后 timeout 置 0 重试
 * @warning data 在拷贝完成前须保持有效
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
int cgrtos_queue_send(cgrtos_queue_t *q, const void *data, tick_t timeout);
/**
 * @brief 从队列接收一条消息
 * @details 队列非空则弹出并可能唤醒 send 等待者；空时按 timeout 挂入 recv_wait_q 并 yield 重试。
 * @param[in]  q       队列；不可为 NULL
 * @param[out] buf     接收缓冲（至少 item_sz 字节）；不可为 NULL
 * @param[in]  timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 结果码
 * @retval pdPASS          成功
 * @retval errPARAM        参数非法
 * @retval errISR          ISR 中调用阻塞路径
 * @retval errQUEUE_EMPTY  非阻塞时队列空
 * @retval pdFAIL          超时或被 delete 唤醒
 * @note 唤醒后 timeout 置 0 重试
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
int cgrtos_queue_receive(cgrtos_queue_t *q, void *buf, tick_t timeout);
/**
 * @brief ISR 向队列发送一条消息（不阻塞）
 * @details 临界区内若未满则 queue_send_internal；已满返回 errQUEUE_FULL；可能唤醒 recv 等待者。
 * @param[in]  q     队列；不可为 NULL
 * @param[in]  data  消息数据（item_sz 字节）；不可为 NULL
 * @param[out] woken 可选；唤醒等待者时置 pdTRUE，否则 yield_from_isr
 * @return 结果码
 * @retval pdPASS         成功
 * @retval errQUEUE_FULL  队列满
 * @retval pdFAIL         参数非法
 * @note woken 语义同 sem_give_from_isr
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_queue_send_from_isr(cgrtos_queue_t *q, const void *data, BaseType_t *woken);
/**
 * @brief ISR 从队列接收一条消息（不阻塞）
 * @details 临界区内若非空则弹出；空则返回 errQUEUE_EMPTY；可能唤醒 send 等待者。
 * @param[in]  q     队列；不可为 NULL
 * @param[out] buf   接收缓冲；不可为 NULL
 * @param[out] woken 可选 woken
 * @return 结果码
 * @retval pdPASS          成功
 * @retval errQUEUE_EMPTY  队列空
 * @retval pdFAIL          参数非法
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_queue_receive_from_isr(cgrtos_queue_t *q, void *buf, BaseType_t *woken);
/**
 * @brief 查询队列中待读消息数
 * @details 临界区内读取 cnt 字段快照。
 * @param[in] q 队列；可为 NULL（返回 0）
 * @return 当前消息条数
 * @retval 0 q 为空或队列为空
 * @retval >0 待读条数
 * @note 只读查询
 * @warning 与 send/receive 并发时为快照
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t cgrtos_queue_messages_waiting(cgrtos_queue_t *q);
/**
 * @brief 删除队列并释放资源
 * @details 唤醒 recv/send 等待者（wake_ok=0）；非静态存储则 free 环形缓冲；回收池槽。
 * @param[in] q 队列；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note storage_static=1 时不 free 缓冲
 * @warning 删除后禁止再使用指针
 * @attention ❌ ISR；✅ 可能切换
 */
int cgrtos_queue_delete(cgrtos_queue_t *q);

/**
 * @brief 创建软定时器（初始为停止状态）
 * @details 从对象池分配；设置 cb/arg/period/periodic；active=0，直至 start 才插入时间轮。
 * @param[in] name     定时器名称；NULL 则默认 "timer"
 * @param[in] cb       到期回调
 * @param[in] arg      回调用户参数
 * @param[in] period   周期或单次时长（tick）；须非 0
 * @param[in] periodic 非 0 表示周期自动重装
 * @return 定时器句柄
 * @retval 非 NULL 成功
 * @retval NULL    池满
 * @note 回调在 Tmr Svc 任务上下文执行
 * @warning period 为 0 时行为未定义
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_timer_t *cgrtos_timer_create(const char *name, timer_cb_t cb, void *arg,
                                    tick_t period, uint8_t periodic);
/**
 * @brief 启动定时器并插入时间轮
 * @details 临界区内 active=1，按 period 调用 wheel_insert。
 * @param[in] timer 定时器句柄；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL timer 为 NULL
 * @note 重复 start 会重新武装
 * @warning 须在任务上下文调用；ISR 请用 start_from_isr
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_timer_start(cgrtos_timer_t *timer);
/**
 * @brief 停止定时器并从时间轮移除
 * @details 临界区内 active=0，wheel_remove_entry 摘除节点。
 * @param[in] timer 定时器句柄；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL timer 为 NULL
 * @note 无
 * @warning ISR 请用 stop_from_isr
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_timer_stop(cgrtos_timer_t *timer);
/**
 * @brief 复位定时器（等价于 stop 后按当前 period 重新 start）
 * @details 委托 cgrtos_timer_start 重新插入时间轮。
 * @param[in] timer 定时器句柄；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL timer 为 NULL
 * @note 无
 * @warning ISR 请用 reset_from_isr
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_timer_reset(cgrtos_timer_t *timer);
/**
 * @brief 修改定时器周期
 * @details 更新 period；若 active 则先从时间轮摘除再按新 period 插入。
 * @param[in] timer  定时器句柄；不可为 NULL
 * @param[in] period 新周期（tick）；须非 0
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 无
 * @warning ISR 请用 change_period_from_isr
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_timer_change_period(cgrtos_timer_t *timer, tick_t period);
/**
 * @brief 删除定时器并回收池槽
 * @details 若 active 则先从时间轮摘除；清零结构体并归还对象池。
 * @param[in] timer 定时器句柄；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 删除后禁止再使用指针
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_timer_delete(cgrtos_timer_t *timer);
/**
 * @brief ISR：投递 START 命令到 Tmr Svc（不在 ISR 改时间轮）
 * @details 委托 timer_cmd_from_isr；守护任务收到后调用 cgrtos_timer_start。
 * @param[in]  timer 定时器句柄；不可为 NULL
 * @param[out] woken 可选 woken
 * @return 结果码
 * @retval pdPASS 命令入队成功
 * @retval pdFAIL 参数非法或命令队列满
 * @note 不在 ISR 内直接操作时间轮
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_timer_start_from_isr(cgrtos_timer_t *timer, BaseType_t *woken);
/**
 * @brief ISR：投递 STOP 命令到 Tmr Svc
 * @details 委托 timer_cmd_from_isr；守护任务调用 cgrtos_timer_stop。
 * @param[in]  timer 定时器句柄；不可为 NULL
 * @param[out] woken 可选 woken
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 失败
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_timer_stop_from_isr(cgrtos_timer_t *timer, BaseType_t *woken);
/**
 * @brief ISR：投递 RESET 命令（按当前 period 重新武装）
 * @details 委托 timer_cmd_from_isr(TIMER_CMD_RESET)。
 * @param[in]  timer 定时器句柄；不可为 NULL
 * @param[out] woken 可选 woken
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 失败
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_timer_reset_from_isr(cgrtos_timer_t *timer, BaseType_t *woken);
/**
 * @brief ISR：投递 CHANGE_PERIOD 命令
 * @details 委托 timer_cmd_from_isr；守护任务调用 cgrtos_timer_change_period。
 * @param[in]  timer  定时器句柄；不可为 NULL
 * @param[in]  period 新周期（须非 0）
 * @param[out] woken  可选 woken
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 失败
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_timer_change_period_from_isr(cgrtos_timer_t *timer, tick_t period,
                                        BaseType_t *woken);
/**
 * @brief 初始化时间轮、命令队列并创建 Tmr Svc 守护任务
 * @details 清零时间轮；创建 CONFIG_TIMER_QUEUE_LEN 命令队列；创建 "Tmr Svc" 任务。
 * @return 无
 * @retval 无
 * @note 须在 cgrtos_start 之前或启动早期调用一次
 * @warning 重复调用可能泄漏资源
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_timer_init(void);

/**
 * @brief 动态创建事件组
 * @details 在 g_egs[] 池分配空槽；清零结构体，设置 in_use=1。
 * @return 事件组指针
 * @retval 非 NULL 成功
 * @retval NULL    池满
 * @note 对象在静态池中，用户不得 free 指针
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_event_group_t *cgrtos_event_group_create(void);
/**
 * @brief 在调用者静态存储上初始化事件组
 * @details 清零结构体，设置 in_use=1；不占用全局池。
 * @param[out] eg 用户提供的事件组结构体；不可为 NULL
 * @return 事件组指针
 * @retval eg     成功
 * @retval NULL   参数非法
 * @note 无
 * @warning eg 生命周期须覆盖使用期
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_event_group_t *cgrtos_event_group_create_static(cgrtos_event_group_t *eg);
/**
 * @brief 置位事件组标志并唤醒匹配等待者
 * @details flags |= 掩码；遍历 wait_q 对满足条件的等待者 unblock；exit 后 yield。
 * @param[in] eg    事件组；不可为 NULL
 * @param[in] flags 要置位的标志掩码
 * @return 置位后的完整 flags
 * @retval 非 0  成功
 * @retval 0     eg 为 NULL
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能切换
 */
event_flags_t cgrtos_event_group_set(cgrtos_event_group_t *eg, event_flags_t flags);
/**
 * @brief ISR 置位事件组标志
 * @details 临界区内 flags |= 掩码；匹配等待者 unblock；通过 woken/yield_from_isr 通知调度。
 * @param[in]  eg    事件组；不可为 NULL
 * @param[in]  flags 置位掩码
 * @param[out] woken 可选 woken；语义同 sem_give_from_isr
 * @return 置位后的完整 flags
 * @retval 非 0  成功
 * @retval 0     eg 为 NULL
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
event_flags_t cgrtos_event_group_set_from_isr(cgrtos_event_group_t *eg,
                                              event_flags_t flags,
                                              BaseType_t *woken);
/**
 * @brief 清除事件组标志位（不唤醒等待者）
 * @details 临界区内 flags &= ~掩码。
 * @param[in] eg    事件组；不可为 NULL
 * @param[in] flags 要清除的标志掩码
 * @return 清除后的完整 flags
 * @retval 非 0  成功
 * @retval 0     eg 为 NULL
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
event_flags_t cgrtos_event_group_clear(cgrtos_event_group_t *eg, event_flags_t flags);
/**
 * @brief ISR 清除事件组标志位（不唤醒等待者）
 * @details 临界区内 flags &= ~掩码；不遍历 wait_q。
 * @param[in] eg    事件组；不可为 NULL
 * @param[in] flags 清除掩码
 * @return 清除后的完整 flags
 * @retval 非 0  成功
 * @retval 0     eg 为 NULL
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
event_flags_t cgrtos_event_group_clear_from_isr(cgrtos_event_group_t *eg,
                                                event_flags_t flags);
/**
 * @brief 等待事件组标志（简化接口）
 * @details 委托 wait_bits，clear_on_exit=0。
 * @param[in] eg       事件组；不可为 NULL
 * @param[in] flags    等待掩码
 * @param[in] wait_all 1=等全部位，0=任一位
 * @param[in] timeout  0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 满足条件时的 flags 快照；失败/超时返回 0
 * @retval 非 0  等到事件
 * @retval 0     超时、参数错误或 eg 为空
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
event_flags_t cgrtos_event_group_wait(cgrtos_event_group_t *eg, event_flags_t flags,
                                      uint8_t wait_all, tick_t timeout);
/**
 * @brief FreeRTOS 风格等待事件组标志
 * @details 条件满足则可选清位并返回；否则挂入 wait_q 并 yield 重试。
 * @param[in] eg            事件组；不可为 NULL
 * @param[in] flags         等待掩码
 * @param[in] clear_on_exit 成功后是否清除相关位
 * @param[in] wait_all      1=等全部位，0=任一位
 * @param[in] timeout       0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 满足条件时的 flags；失败/超时返回 0
 * @retval 非 0  成功
 * @retval 0     超时或参数错误
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
event_flags_t cgrtos_event_group_wait_bits(cgrtos_event_group_t *eg, event_flags_t flags,
                                           uint8_t clear_on_exit, uint8_t wait_all,
                                           tick_t timeout);
/**
 * @brief 读取事件组当前 flags
 * @details 临界区内快照 flags 字段。
 * @param[in] eg 事件组；可为 NULL
 * @return 当前 flags 位图
 * @retval 0 eg 为空
 * @retval >0 当前标志
 * @note 只读查询
 * @warning 与 set/clear 并发时为快照
 * @attention ✅ ISR；❌ 不阻塞
 */
event_flags_t cgrtos_event_group_get(cgrtos_event_group_t *eg);
/**
 * @brief 删除事件组并回收池槽
 * @details 唤醒 wait_q 等待者（wake_ok=0）；清零结构体并归还池槽。
 * @param[in] eg 事件组；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 删除后禁止再使用指针
 * @warning 无
 * @attention ❌ ISR；✅ 可能切换
 */
int cgrtos_event_group_delete(cgrtos_event_group_t *eg);

/**
 * @defgroup cgrtos_stream StreamBuffer / MessageBuffer
 * @brief 字节流与整消息缓冲；可加入 QueueSet
 * @{
 */
/**
 * @brief 创建 StreamBuffer
 * @details 池分配槽位；堆上分配 size 字节环形缓冲；设置 trigger（0 视为 1）。
 * @param[in] size    环形缓冲容量（字节）；至少 2
 * @param[in] trigger 可读字节 ≥ trigger 时唤醒接收者；0 视为 1
 * @return StreamBuffer 句柄
 * @retval 非 NULL 成功
 * @retval NULL    池满、参数非法或堆不足
 * @note 可加入 QueueSet
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_stream_buffer_t *cgrtos_stream_buffer_create(uint32_t size, uint32_t trigger);
/**
 * @brief 向 StreamBuffer 写入字节流（可部分写入）
 * @details 空间足够则写入并可能唤醒接收者；不足时按 timeout 阻塞重试。
 * @param[in]  sb      StreamBuffer；不可为 NULL
 * @param[in]  data    源数据；不可为 NULL
 * @param[in]  len     期望写入字节数
 * @param[in]  timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 实际写入字节数
 * @retval >0 部分或全部写入
 * @retval 0  超时、参数错误或不可写
 * @note 可能只写部分数据
 * @warning data 在拷贝完成前须保持有效
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
size_t cgrtos_stream_buffer_send(cgrtos_stream_buffer_t *sb, const void *data,
                                 size_t len, tick_t timeout);
/**
 * @brief ISR 向 StreamBuffer 发送字节流（不阻塞，可部分写入）
 * @details 临界区内尽可能写入；空间不足则只写部分；可能唤醒接收者。
 * @param[in]  sb    StreamBuffer；不可为 NULL
 * @param[in]  data  源数据；不可为 NULL
 * @param[in]  len   期望长度
 * @param[out] woken 可选 woken
 * @return 实际写入字节数
 * @retval >0 已写入字节数
 * @retval 0  参数错误或队列满
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
size_t cgrtos_stream_buffer_send_from_isr(cgrtos_stream_buffer_t *sb, const void *data,
                                          size_t len, BaseType_t *woken);
/**
 * @brief 从 StreamBuffer 读取字节流（可部分读取）
 * @details 数据 ≥ trigger 或 timeout 到期则读出；不足时可阻塞。
 * @param[in]  sb      StreamBuffer；不可为 NULL
 * @param[out] buf     接收缓冲；不可为 NULL
 * @param[in]  len     最大读取字节数
 * @param[in]  timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 实际读出字节数
 * @retval >0 已读出字节数
 * @retval 0  超时、参数错误或数据不足
 * @note 可能只读部分数据
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
size_t cgrtos_stream_buffer_recv(cgrtos_stream_buffer_t *sb, void *buf, size_t len,
                                 tick_t timeout);
/**
 * @brief ISR 从 StreamBuffer 接收字节流（不阻塞）
 * @details 临界区内尽可能读出；无足够数据则返回已读部分或 0。
 * @param[in]  sb    StreamBuffer；不可为 NULL
 * @param[out] buf   接收缓冲；不可为 NULL
 * @param[in]  len   最大长度
 * @param[out] woken 可选 woken
 * @return 实际读出字节数
 * @retval >0 已读出字节数
 * @retval 0  无数据或参数错误
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
size_t cgrtos_stream_buffer_recv_from_isr(cgrtos_stream_buffer_t *sb, void *buf,
                                          size_t len, BaseType_t *woken);
/**
 * @brief 清空 StreamBuffer 并唤醒等待者
 * @details 重置读写指针与计数；唤醒 send/recv 等待者。
 * @param[in] sb StreamBuffer；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 无
 * @warning 丢弃全部未读数据
 * @attention ❌ ISR；✅ 可能切换
 */
int cgrtos_stream_buffer_reset(cgrtos_stream_buffer_t *sb);
/**
 * @brief 删除 StreamBuffer 并释放堆缓冲
 * @details 唤醒等待者；free 环形缓冲；回收池槽。
 * @param[in] sb StreamBuffer；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 删除后禁止再使用指针
 * @warning 无
 * @attention ❌ ISR；✅ 可能切换
 */
int cgrtos_stream_buffer_delete(cgrtos_stream_buffer_t *sb);
/**
 * @brief 查询 StreamBuffer 当前可读字节数
 * @details 临界区内读取可用字节计数快照。
 * @param[in] sb StreamBuffer；可为 NULL
 * @return 可读字节数
 * @retval 0 sb 为空或无数据
 * @retval >0 可读字节
 * @note 只读查询
 * @warning 与 send/recv 并发时为快照
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t cgrtos_stream_buffer_bytes_available(cgrtos_stream_buffer_t *sb);
/**
 * @brief 查询 StreamBuffer 当前可写空闲字节数
 * @details 临界区内计算 capacity - used 快照。
 * @param[in] sb StreamBuffer；可为 NULL
 * @return 可写空闲字节数
 * @retval 0 sb 为空或已满
 * @retval >0 剩余空间
 * @note 只读查询
 * @warning 与 send/recv 并发时为快照
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t cgrtos_stream_buffer_spaces_available(cgrtos_stream_buffer_t *sb);

/**
 * @brief 创建 MessageBuffer（整消息原子收发）
 * @details 委托 stream_buffer_create；size 须容纳最长消息 + 2 字节长度头。
 * @param[in] size 缓冲总容量（字节）
 * @return MessageBuffer 句柄
 * @retval 非 NULL 成功
 * @retval NULL    池满或堆不足
 * @note 每条消息带 2 字节 LE 长度前缀
 * @warning size 过小将无法发送较长消息
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_message_buffer_t *cgrtos_message_buffer_create(uint32_t size);
/**
 * @brief 发送一整条消息
 * @details 空间足够则原子写入长度头 + 载荷；不足则阻塞或返回 0。
 * @param[in]  mb      MessageBuffer；不可为 NULL
 * @param[in]  data    载荷；不可为 NULL
 * @param[in]  len     载荷长度（≤ 0xFFFF）
 * @param[in]  timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 成功时为 payload 长度，失败为 0
 * @retval len 发送成功
 * @retval 0   超时、参数错误或空间不足
 * @note 不部分写入
 * @warning data 在拷贝完成前须保持有效
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
size_t cgrtos_message_buffer_send(cgrtos_message_buffer_t *mb, const void *data,
                                  size_t len, tick_t timeout);
/**
 * @brief 接收一整条消息
 * @details 等待完整消息；buf_len 过小时丢弃该消息并返回 0。
 * @param[in]  mb      MessageBuffer；不可为 NULL
 * @param[out] buf     接收缓冲；不可为 NULL
 * @param[in]  buf_len 缓冲容量
 * @param[in]  timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 消息载荷长度；超时/空/失败为 0
 * @retval >0 成功接收的载荷长度
 * @retval 0  超时、无消息、buf 过小或参数错误
 * @note 无
 * @warning buf_len 须 ≥ 消息长度
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
size_t cgrtos_message_buffer_recv(cgrtos_message_buffer_t *mb, void *buf, size_t buf_len,
                                  tick_t timeout);
/**
 * @brief ISR 发送一整条消息（空间不足返回 0，不部分写入）
 * @details 临界区内检查空间；足够则原子写入；可能唤醒接收者。
 * @param[in]  mb    MessageBuffer；不可为 NULL
 * @param[in]  data  载荷；不可为 NULL
 * @param[in]  len   载荷长度（≤ 0xFFFF）
 * @param[out] woken 可选 woken
 * @return 成功为 len，失败为 0
 * @retval len 发送成功
 * @retval 0   空间不足或参数错误
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
size_t cgrtos_message_buffer_send_from_isr(cgrtos_message_buffer_t *mb, const void *data,
                                           size_t len, BaseType_t *woken);
/**
 * @brief ISR 接收一整条消息
 * @details 无完整消息或 buf 过小时返回 0；否则读出整包载荷。
 * @param[in]  mb      MessageBuffer；不可为 NULL
 * @param[out] buf     接收缓冲；不可为 NULL
 * @param[in]  buf_len 缓冲容量
 * @param[out] woken   可选 woken
 * @return 载荷长度；失败为 0
 * @retval >0 成功
 * @retval 0  无消息、buf 过小或参数错误
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
size_t cgrtos_message_buffer_recv_from_isr(cgrtos_message_buffer_t *mb, void *buf,
                                           size_t buf_len, BaseType_t *woken);
/**
 * @brief 删除 MessageBuffer
 * @details 委托 cgrtos_stream_buffer_delete。
 * @param[in] mb MessageBuffer；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 删除后禁止再使用指针
 * @warning 无
 * @attention ❌ ISR；✅ 可能切换
 */
int cgrtos_message_buffer_delete(cgrtos_message_buffer_t *mb);
/** @} */

/**
 * @defgroup cgrtos_qset QueueSet
 * @brief 多 IPC 对象就绪选择（类似 select）
 * @{
 */
/**
 * @brief 创建 QueueSet
 * @details 在 g_qsets[] 池分配槽位；length 为预留参数，实际容量为 CGRTOS_QUEUE_SET_LENGTH。
 * @param[in] length 预留参数（当前未使用）
 * @return QueueSet 句柄
 * @retval 非 NULL 成功
 * @retval NULL    池满
 * @note 成员对象须通过 add_* 注册
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_queue_set_t *cgrtos_queue_set_create(uint32_t length);
/**
 * @brief 将队列加入 QueueSet
 * @details 绑定 q->qset；若已加入其它 set 则失败。
 * @param[in] set QueueSet；不可为 NULL
 * @param[in] q   队列；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或已加入其它 set
 * @note 无
 * @warning 同一对象只能属于一个 set
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_queue_set_add_queue(cgrtos_queue_set_t *set, cgrtos_queue_t *q);
/**
 * @brief 将信号量加入 QueueSet
 * @details 绑定 sem->qset；若已加入其它 set 则失败。
 * @param[in] set QueueSet；不可为 NULL
 * @param[in] sem 信号量；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或已加入其它 set
 * @note 无
 * @warning 同一对象只能属于一个 set
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_queue_set_add_sem(cgrtos_queue_set_t *set, cgrtos_sem_t *sem);
/**
 * @brief 将 StreamBuffer/MessageBuffer 加入 QueueSet
 * @details 绑定 sb->qset；若已加入其它 set 则失败。
 * @param[in] set QueueSet；不可为 NULL
 * @param[in] sb  StreamBuffer 或 MessageBuffer；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或已加入其它 set
 * @note MessageBuffer 与 StreamBuffer 共用底层类型
 * @warning 同一对象只能属于一个 set
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_queue_set_add_stream(cgrtos_queue_set_t *set, cgrtos_stream_buffer_t *sb);
/**
 * @brief 从 QueueSet 移除成员
 * @details 按对象指针匹配并清空其 qset 反向引用。
 * @param[in] set QueueSet；不可为 NULL
 * @param[in] obj 成员对象指针（queue/sem/stream）；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或未找到
 * @note 不删除成员对象本身
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_queue_set_remove(cgrtos_queue_set_t *set, void *obj);
/**
 * @brief 阻塞直到 QueueSet 中任一成员就绪
 * @details 就绪环有成员则 pop 返回指针；否则挂入 wait_q 并 yield 重试。
 * @param[in] set     QueueSet；不可为 NULL
 * @param[in] timeout 0=非阻塞；portMAX_DELAY=永久；其它=相对 tick
 * @return 就绪成员对象指针
 * @retval 非 NULL 有成员就绪
 * @retval NULL    超时或参数错误
 * @note 返回后仍须对该对象再 take/recv；不自动消费数据
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞并切换
 */
void *cgrtos_queue_set_select(cgrtos_queue_set_t *set, tick_t timeout);
/**
 * @brief 删除 QueueSet
 * @details 解除所有成员 qset 引用；回收池槽；不删除成员对象。
 * @param[in] set QueueSet；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 删除后禁止再使用指针
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_queue_set_delete(cgrtos_queue_set_t *set);
/**
 * @brief 成员就绪时内部 poke（由 send/give/stream_send 调用）
 * @details 将成员推入就绪环并唤醒 select 等待者。
 * @param[in] set QueueSet；不可为 NULL
 * @param[in] obj 就绪成员对象指针；不可为 NULL
 * @return 无
 * @retval 无
 * @note 应用层一般无需直接调用
 * @warning 须在持有临界区或 IPC 路径内调用
 * @attention ✅ ISR；❌ 不阻塞；✅ 可能唤醒 select 等待者
 */
void cgrtos_queue_set_poke(cgrtos_queue_set_t *set, void *obj);
/** @} */

/**
 * @defgroup cgrtos_fs_api RAM FS API
 * @ingroup cgrtos_fs
 * @{
 */
/**
 * @brief 初始化 RAM 文件系统根 inode 与 FS 互斥
 * @details 创建 g_fs_mtx；初始化 inode 0 为根目录 `/`；cgrtos_init 已调用，可重复安全调用。
 * @return 无
 * @retval 无
 * @note 重复调用安全
 * @warning 须在堆初始化之后调用
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_fs_init(void);
/**
 * @brief 打开或创建文件
 * @details 解析绝对路径；O_CREAT 时创建普通文件；O_TRUNC 截断；分配 fd 槽。
 * @param[in] path  绝对路径（须以 `/` 开头）；不可为 NULL
 * @param[in] flags CGRTOS_O_* 组合
 * @return 文件描述符
 * @retval ≥0 成功
 * @retval -1  失败
 * @note 全局 FS 互斥串行化
 * @warning 目录路径不可打开为文件
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_open(const char *path, int flags);
/**
 * @brief 关闭文件描述符
 * @details 释放 fd 槽；不删除 inode 数据。
 * @param[in] fd 文件描述符
 * @return 结果码
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_close(int fd);
/**
 * @brief 从打开的文件读取数据
 * @details 从 fd 当前 pos 拷贝至多 len 字节；EOF 返回 0。
 * @param[in]  fd  文件描述符
 * @param[out] buf 接收缓冲；不可为 NULL
 * @param[in]  len 期望读取字节数
 * @return 实际读出字节数
 * @retval ≥0 成功（0 表示 EOF）
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_read(int fd, void *buf, size_t len);
/**
 * @brief 向打开的文件写入数据
 * @details 可增长至 CGRTOS_FS_MAX_FILE_BYTES；O_APPEND 追加到末尾。
 * @param[in] fd  文件描述符
 * @param[in] buf 源数据；不可为 NULL
 * @param[in] len 写出字节数
 * @return 实际写出字节数
 * @retval ≥0 成功
 * @retval -1 失败
 * @note 可能触发堆扩容
 * @warning 只读打开时失败
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_write(int fd, const void *buf, size_t len);
/**
 * @brief 移动文件读写位置
 * @details whence：0=SET，1=CUR，2=END。
 * @param[in] fd     文件描述符
 * @param[in] off    偏移量
 * @param[in] whence 基准（0/1/2）
 * @return 新偏移
 * @retval ≥0 成功
 * @retval -1 失败
 * @note 负偏移会被钳制为 0
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
long cgrtos_fs_lseek(int fd, long off, int whence);
/**
 * @brief 删除普通文件（unlink）
 * @details 从父目录移除 dentry 并释放 inode。
 * @param[in] path 绝对路径；不可为 NULL
 * @return 结果码
 * @retval 0  成功
 * @retval -1 失败
 * @note 不能删除目录
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_unlink(const char *path);
/**
 * @brief 创建目录
 * @details 父目录须已存在；路径不得已存在。
 * @param[in] path 绝对路径；不可为 NULL
 * @return 结果码
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 不会递归创建父目录
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_mkdir(const char *path);
/**
 * @brief 删除空目录
 * @details 目录须无任何 dentry；从父目录移除。
 * @param[in] path 绝对路径；不可为 NULL
 * @return 结果码
 * @retval 0  成功
 * @retval -1 失败（非空或不存在）
 * @note 无
 * @warning 非空目录无法删除
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_rmdir(const char *path);
/**
 * @brief 查询路径对应 inode 的类型与大小
 * @details 填充 mode（CGRTOS_S_IFREG/IFDIR）与 size。
 * @param[in]  path 绝对路径；不可为 NULL
 * @param[out] st   输出 stat；不可为 NULL
 * @return 结果码
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_stat(const char *path, cgrtos_stat_t *st);
/**
 * @brief 打开目录以供 readdir 迭代
 * @details 分配 g_dirs[] 槽位；记录目录 inode 与 pos=0。
 * @param[in] path 绝对路径；须为已存在目录
 * @return 目录句柄
 * @retval 非 NULL 成功
 * @retval NULL    失败或目录句柄池满（最多 8 个）
 * @note 须 paired closedir
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
cgrtos_dir_t *cgrtos_fs_opendir(const char *path);
/**
 * @brief 读取目录下一项
 * @details 解析打包 dentry；推进 dir->pos。
 * @param[in]  dir 目录句柄；不可为 NULL
 * @param[out] out 输出目录项；不可为 NULL
 * @return 读取结果
 * @retval 1  有项
 * @retval 0  结束（EOF）
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_readdir(cgrtos_dir_t *dir, cgrtos_dirent_t *out);
/**
 * @brief 关闭目录句柄
 * @details 释放 g_dirs[] 槽位。
 * @param[in] dir 目录句柄；不可为 NULL
 * @return 结果码
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞（FS 互斥）
 */
int cgrtos_fs_closedir(cgrtos_dir_t *dir);
/**
 * @brief 强制格式化 RAM FS（清空全部 inode/fd，重建根目录）
 * @details 关闭所有打开 fd 与目录句柄，释放堆数据后重新初始化根。
 * @return 0 成功；-1 失败
 * @retval 0  卷已清空
 * @retval -1 内部错误
 * @note 幂等；会丢弃全部用户数据
 * @warning 危险操作；CLI 须交互确认
 * @attention ❌ ISR；✅ 可能阻塞（互斥）
 */
int cgrtos_fs_format(void);
/**
 * @brief RAM FS 刷盘（内存后端为空操作屏障）
 * @details 持锁确保无进行中的写，作为 VFS sync 后端。
 * @return 0
 * @retval 0 成功
 * @note 无持久化介质时无实际 I/O
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
int cgrtos_fs_sync(void);
/**
 * @brief 查询 RAM FS 用量
 * @details 统计 used inode 与 data size，按 512B 换算逻辑块。
 * @param[out] st 输出结构；不可为 NULL
 * @return 0 成功；-1 参数非法
 * @retval 0  已填充
 * @retval -1 st 为空
 * @note 供 df/fbench 使用
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
int cgrtos_fs_statfs(cgrtos_statfs_t *st);
/**
 * @brief 重命名/移动文件或空目录（同卷内）
 * @details 从旧父目录摘除 dentry，挂到新父目录；禁止覆盖已存在目标。
 * @param[in] oldpath 源绝对路径
 * @param[in] newpath 目标绝对路径
 * @return 0 成功；-1 失败
 * @retval 0  已移动
 * @retval -1 路径非法、跨非法、目标已存在或非空目录策略失败
 * @note 目录仅支持空目录移动（与 rmdir 一致约束）
 * @warning 打开中的 fd 仍指向同一 inode
 * @attention ❌ ISR；✅ 可能阻塞
 */
int cgrtos_fs_rename(const char *oldpath, const char *newpath);
/** @} */

/**
 * @brief TLSF 堆分配
 * @details 从内核 TLSF 堆分配 size 字节；失败返回 NULL。
 * @param[in] size 请求字节数；0 行为以实现为准
 * @return 用户可用指针
 * @retval 非 NULL 成功
 * @retval NULL    堆不足或参数非法
 * @note 与 cgrtos_free 成对；对象生命周期须由调用方管理
 * @warning 非 ISR 安全；持 g_klock 或堆锁，ISR 中调用可能死锁
 * @attention ❌ ISR；❌ 不阻塞
 */
void *cgrtos_malloc(unsigned long size);
/**
 * @brief TLSF 堆分配并清零
 * @details 等价 malloc(count*size) 后 memset 0；乘积溢出时返回 NULL。
 * @param[in] count 元素个数
 * @param[in] size  单元素字节数
 * @return 用户指针或 NULL
 * @retval 非 NULL 成功
 * @retval NULL    溢出/堆不足
 * @note 无
 * @warning 同 cgrtos_malloc，禁止 ISR 调用
 * @attention ❌ ISR；❌ 不阻塞
 */
void *cgrtos_calloc(unsigned long count, unsigned long size);
/**
 * @brief 释放 TLSF 堆块
 * @details ptr 为 NULL 时安全无操作；否则归还 TLSF 并可选 redzone 校验。
 * @param[in] ptr cgrtos_malloc/calloc 返回值；NULL 忽略
 * @return 无
 * @retval 无
 * @note 禁止双重释放；禁止释放非堆指针
 * @warning 非 ISR 安全
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_free(void *ptr);
/**
 * @brief 查询当前空闲堆字节数
 * @details 读取 TLSF 统计；诊断/水位监控用。
 * @return 当前可用堆字节
 * @retval >=0 空闲字节估测
 * @note 快照值，并发分配时可能瞬时变化
 * @warning 无
 * @attention ✅ 只读（短临界）；❌ 不阻塞
 */
unsigned long cgrtos_get_free_heap(void);
/**
 * @brief 查询历史最小空闲堆字节数
 * @details 自启动以来观测到的最低 free_heap 水位。
 * @return 历史最小空闲堆
 * @retval >=0 字节数
 * @note 用于检测内存泄漏趋势
 * @warning 无
 * @attention ✅ 只读（短临界）；❌ 不阻塞
 */
unsigned long cgrtos_get_min_free_heap(void);

#if CONFIG_USE_HOOKS
/**
 * @brief 注册 idle 任务循环钩子
 * @details 每核 idle 循环入口调用；NULL 清除钩子。
 * @param[in] hook 回调；可为 NULL
 * @return 无
 * @retval 无
 * @note 钩子须短小非阻塞；勿在 idle 中阻塞或分配堆
 * @warning 钩子内阻塞会饿死低优先级任务
 * @attention ❌ ISR；❌ 不阻塞（钩子本身）
 */
void cgrtos_set_idle_hook(cgrtos_hook_fn_t hook);
/**
 * @brief 注册系统 tick 钩子
 * @details hart0 的 cgrtos_tick_handler 路径调用；NULL 清除。
 * @param[in] hook 回调；可为 NULL
 * @return 无
 * @retval 无
 * @note 在 tick ISR 上下文执行，须极短
 * @warning 延长 tick 钩子影响全局节拍精度
 * @attention ❌ 任务上下文注册；❌ 不阻塞
 */
void cgrtos_set_tick_hook(cgrtos_hook_fn_t hook);
/**
 * @brief 注册堆分配失败钩子
 * @details cgrtos_malloc/calloc 失败时调用；NULL 清除。
 * @param[in] hook 回调，参数为请求字节数
 * @return 无
 * @retval 无
 * @note 钩子内勿再次 malloc
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_malloc_failed_hook(cgrtos_malloc_failed_hook_t hook);
#endif
/**
 * @brief 注册断言失败钩子
 * @details cgrtos_assert_failed 在 halt 前调用；NULL 则仅默认打印。
 * @param[in] hook 回调 (file, line)；可为 NULL
 * @return 无
 * @retval 无
 * @note 钩子返回后内核仍可能进入死循环
 * @warning 勿在钩子内再触发 assert
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_assert_hook(cgrtos_assert_hook_t hook);
/**
 * @brief 注册栈溢出检测钩子
 * @details 金丝雀失败或高水位告警时调用；NULL 清除。
 * @param[in] hook 回调，参数为溢出任务 TCB
 * @return 无
 * @retval 无
 * @note 默认路径随后仍会 assert halt
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_stack_overflow_hook(cgrtos_stack_overflow_hook_t hook);

#if CONFIG_USE_HOOKS
/**
 * @brief 注册任务创建钩子
 * @details cgrtos_task_create 成功后调用；NULL 清除。
 * @param[in] hook 回调
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_task_create_hook(cgrtos_hook_fn_t hook);
/**
 * @brief 注册任务删除钩子
 * @details cgrtos_task_delete 标记删除时调用；NULL 清除。
 * @param[in] hook 回调
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_task_delete_hook(cgrtos_hook_fn_t hook);
/**
 * @brief 注册 ISR 非法 API 调用钩子
 * @details CONFIG_ISR_API_GUARD 命中 errISR 时调用；NULL 清除。
 * @param[in] hook 回调
 * @return 无
 * @retval 无
 * @note 用于测试/诊断非法 FromISR 用法
 * @warning 无
 * @attention ❌ 任务上下文注册；❌ 不阻塞
 */
void cgrtos_set_isr_api_hook(cgrtos_hook_fn_t hook);
/**
 * @brief 注册调度器内部错误钩子
 * @details 调度 invariant 失败等路径调用；NULL 清除。
 * @param[in] hook 回调
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_sched_error_hook(cgrtos_hook_fn_t hook);
/**
 * @brief 注册 IRQ/异常诊断钩子
 * @details 同步异常或未处理 IRQ 路径可选调用；NULL 清除。
 * @param[in] hook 回调
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_irq_exception_hook(cgrtos_hook_fn_t hook);
/**
 * @brief 注册看门狗超时钩子
 * @details 看门狗未 kick 触发时调用；NULL 清除。
 * @param[in] hook 回调
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_watchdog_hook(cgrtos_hook_fn_t hook);
/**
 * @brief 注册临界区超时钩子
 * @details CONFIG_IRQ_DISABLE_MONITOR 检测到关中断/临界区超时时调用；NULL 清除。
 * @param[in] hook 回调
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_crit_overrun_hook(cgrtos_hook_fn_t hook);
#if CONFIG_IDLE_SLEEP_HOOK
/**
 * @brief 注册 idle 低功耗睡眠钩子
 * @details idle 循环在 WFI/忙等前调用；与 idle_hook 分离；NULL 清除。
 * @param[in] hook 回调
 * @return 无
 * @retval 无
 * @note 须可从中断唤醒
 * @warning 睡眠过久可能延迟 tick/IPI 响应
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_set_idle_sleep_hook(cgrtos_hook_fn_t hook);
#endif
#endif

/* --- 模块3 内存池 --- */
typedef struct cgrtos_mempool cgrtos_mempool_t;
/**
 * @brief 在调用者 storage 上创建固定块内存池
 * @details 块大小对齐后链成 freelist；不占用 TLSF 堆（storage 由调用方提供）。
 * @param[in] storage     连续内存；至少 block_size*block_count 字节
 * @param[in] block_size  单块字节数；须 >0 且对齐
 * @param[in] block_count 块数量；须 >0
 * @return 池句柄
 * @retval 非 NULL 成功
 * @retval NULL    参数非法
 * @note 池对象本身通常静态分配；delete 不释放 storage
 * @warning storage 生命周期须覆盖整个池使用期
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_mempool_t *cgrtos_mempool_create(void *storage, uint32_t block_size,
                                        uint32_t block_count);
/**
 * @brief 从内存池分配一块
 * @details O(1) 弹出 freelist 头；池空返回 NULL。
 * @param[in] pool 池句柄；不可为 NULL
 * @return 块指针
 * @retval 非 NULL 成功
 * @retval NULL    池空或 pool 无效
 * @note 块须用 cgrtos_mempool_free 归还同一池
 * @warning 禁止跨池 free
 * @attention ❌ ISR（若池无锁保护）；❌ 不阻塞
 */
void *cgrtos_mempool_alloc(cgrtos_mempool_t *pool);
/**
 * @brief 归还块到内存池
 * @details 校验 ptr 属于该池后推回 freelist。
 * @param[in] pool 池句柄
 * @param[in] ptr  alloc 返回值
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或 ptr 不属于该池
 * @note 无
 * @warning 双重 free 可能导致链表损坏
 * @attention ❌ ISR（若池无锁保护）；❌ 不阻塞
 */
int cgrtos_mempool_free(cgrtos_mempool_t *pool, void *ptr);
/**
 * @brief 销毁内存池
 * @details 标记池无效；不释放 storage 内存。
 * @param[in] pool 池句柄
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL pool 无效
 * @note 销毁后禁止 alloc/free
 * @warning 仍有 outstanding 块时行为以实现为准
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_mempool_delete(cgrtos_mempool_t *pool);
/**
 * @brief 查询池中空闲块数
 * @details 遍历或计数 freelist。
 * @param[in] pool 池句柄
 * @return 空闲块数
 * @retval >=0 有效计数
 * @retval 0    pool 无效
 * @note 快照值
 * @warning 无
 * @attention ✅ 只读；❌ 不阻塞
 */
uint32_t cgrtos_mempool_free_count(cgrtos_mempool_t *pool);

/* --- 模块4 安全 --- */
/**
 * @brief ISR 上下文拒绝阻塞 API 检测
 * @details CONFIG_ISR_API_GUARD 启用时，在 ISR 中调用阻塞路径前调用；命中则 errISR 并触发 isr_api_hook。
 * @return 非 0 表示当前在 ISR 且应拒绝
 * @retval 1 在 ISR 中
 * @retval 0 任务上下文
 * @note 由 IPC/调度内部调用
 * @warning 无
 * @attention ✅ 可在任意上下文查询；❌ 不阻塞
 */
int cgrtos_reject_blocking_in_isr(void);
/**
 * @brief 临界区进入安全监控记账
 * @details CONFIG_IRQ_DISABLE_MONITOR 启用时记录 enter 时刻与嵌套。
 * @param[in] cpu 逻辑核号
 * @return 无
 * @retval 无
 * @note 由 cgrtos_enter_critical 内部调用
 * @warning 无
 * @attention ✅ 临界区内；❌ 不阻塞
 */
void cgrtos_safety_on_crit_enter(uint8_t cpu);
/**
 * @brief 临界区退出安全监控记账
 * @details 计算持锁时长；超阈值触发 crit_overrun_hook。
 * @param[in] cpu 逻辑核号
 * @return 无
 * @retval 无
 * @note 由 cgrtos_exit_critical 内部调用
 * @warning 无
 * @attention ✅ 临界区内；❌ 不阻塞
 */
void cgrtos_safety_on_crit_exit(uint8_t cpu);
/**
 * @brief 查询临界区超时累计次数
 * @details 自启动以来 crit overrun 告警计数。
 * @return 超时次数
 * @retval >=0 计数
 * @note 诊断用
 * @warning 无
 * @attention ✅ 只读；❌ 不阻塞
 */
uint32_t cgrtos_crit_overrun_count(void);
/**
 * @brief 查询某核临界区最大持有时长（周期）
 * @details 以 CPU 周期或等价计时单位记录峰值。
 * @param[in] cpu 逻辑核号
 * @return 最大周期数
 * @retval >=0 峰值
 * @note 诊断用
 * @warning 无
 * @attention ✅ 只读；❌ 不阻塞
 */
uint64_t cgrtos_crit_max_cycles(uint8_t cpu);
/**
 * @brief 致命错误处理（打印后 halt）
 * @details 输出 reason/code 后进入死循环或 watchdog。
 * @param[in] reason 原因字符串；可为 NULL
 * @param[in] code   错误码
 * @return 无（不返回）
 * @retval 无
 * @note 无
 * @warning 调用后不返回
 * @attention ❌ ISR 慎用（可能 UART 阻塞）；❌ 不返回
 */
void cgrtos_fatal_error(const char *reason, int code);
/**
 * @brief 喂看门狗（重置超时计数）
 * @details 应用/idle 周期调用以防 watchdog_hook 触发。
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ✅ 任务/ISR 均可；❌ 不阻塞
 */
void cgrtos_watchdog_kick(void);

/* --- 模块4 MPU 桩 --- */
typedef struct {
    uintptr_t base;
    uint32_t  size;
    uint32_t  attr; /* bit0=R bit1=W bit2=X */
} cgrtos_mpu_region_t;
/**
 * @brief 初始化 MPU 子系统（软件桩或硬件）
 * @details CONFIG_USE_MPU=0 时为 no-op 成功；=1 时配置硬件 region 表。
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 硬件初始化失败
 * @note 由 cgrtos_init 或板级启动调用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_mpu_init(void);
/**
 * @brief 配置 MPU region 槽
 * @details 写入 base/size/attr；idx 须 < CONFIG_MPU_MAX_REGIONS。
 * @param[in] idx region 索引
 * @param[in] r   region 描述；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或硬件拒绝
 * @note 无
 * @warning 错误配置可能导致访问 fault
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_mpu_configure_region(uint32_t idx, const cgrtos_mpu_region_t *r);
/**
 * @brief 为任务启用 MPU 隔离
 * @details 按任务栈/代码 region 切换 MPU 上下文（桩或硬件）。
 * @param[in] id 任务 ID
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 任务不存在或 MPU 未初始化
 * @note 切换上下文时由调度器调用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_mpu_enable_task_isolation(task_id_t id);
/**
 * @brief 为任务禁用 MPU 隔离
 * @details 恢复默认 region 映射。
 * @param[in] id 任务 ID
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 任务不存在
 * @note 任务删除或切换全访问模式时调用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_mpu_disable_task_isolation(task_id_t id);

/* --- 模块6 日志 / 任务导出 --- */
/**
 * @brief 设置全局日志级别阈值
 * @details level 以下级别被过滤；0=NONE 关闭全部。
 * @param[in] level CGRTOS_LOG_NONE..DEBUG
 * @return 无
 * @retval 无
 * @note 影响后续 cgrtos_log 与 CGRTOS_LOG* 宏
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_log_set_level(int level);
/**
 * @brief 读取当前日志级别阈值
 * @return 当前 level
 * @retval 0..4 级别值
 * @note 无
 * @warning 无
 * @attention ✅ 只读；❌ 不阻塞
 */
int cgrtos_log_get_level(void);
/**
 * @brief 输出分级内核日志
 * @details level 低于阈值则丢弃；经 UART/console 输出 tag:msg。
 * @param[in] level 日志级别
 * @param[in] tag   模块标签；可为 NULL
 * @param[in] msg   消息字符串；可为 NULL
 * @return 无
 * @retval 无
 * @note CONFIG_USE_KLOG=0 时为空操作
 * @warning 可能阻塞等待 TX FIFO；非 ISR 安全
 * @attention ❌ ISR；✅ 可能阻塞（UART）
 */
void cgrtos_log(int level, const char *tag, const char *msg);

typedef struct {
    task_id_t       id;
    char            name[CGRTOS_TASK_NAME_MAX];
    uint8_t         prio;
    task_state_t    state;
    sched_policy_t  policy;
    tick_t          exec_ticks;
    uint32_t        stack_hwm; /* words unused from bottom */
} cgrtos_task_info_t;

/**
 * @brief 导出任务列表到缓冲区
 * @details 遍历 g_tasks 填充 out[]；跳过 DELETED/空闲槽。
 * @param[out] out 输出数组；NULL 时仅返回计数
 * @param[in]  max 数组容量（条数）
 * @return 写入条数
 * @retval >=0 实际写入或任务总数（out 为 NULL）
 * @note 快照无锁或短临界，SMP 下可能略有滞后
 * @warning out 不足时截断
 * @attention ❌ ISR 慎用；❌ 不阻塞
 */
uint32_t cgrtos_task_list_export(cgrtos_task_info_t *out, uint32_t max);

/* -------------------------------------------------------------------------- */
/* Trace & object query                                                        */
/* -------------------------------------------------------------------------- */

/** @brief Trace 事件类型 */
typedef enum {
    CGRTOS_TRACE_SWITCH   = 1, /**< 上下文切换：a0=from_id a1=to_id */
    CGRTOS_TRACE_ISR_ENTER = 2, /**< ISR 进入：a0=nest a1=0 */
    CGRTOS_TRACE_ISR_EXIT  = 3, /**< ISR 退出 */
    CGRTOS_TRACE_BLOCK     = 4, /**< 阻塞：a0=task_id a1=reason */
    CGRTOS_TRACE_UNBLOCK   = 5, /**< 唤醒：a0=task_id a1=wake_ok */
    CGRTOS_TRACE_USER      = 100 /**< 用户自定义起点 */
} cgrtos_trace_type_t;

/**
 * @brief 记录一条 Trace 事件
 * @details ISR 安全环缓冲写入。
 * @param[in] type 事件类型
 * @param[in] a0   参数0
 * @param[in] a1   参数1
 * @return 无
 * @retval 无
 * @note CONFIG_USE_TRACE=0 时为空操作
 * @warning 高频率可能覆盖旧记录
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_trace_event(uint16_t type, uint32_t a0, uint32_t a1);
/** @brief 清空 Trace @attention ❌ ISR 慎用；❌ 不阻塞 */
void cgrtos_trace_reset(void);
/**
 * @brief 导出 Trace（每条 4×uint32：ts, meta, a0, a1）
 * @param[out] out 缓冲
 * @param[in]  max 最大条数
 * @return 实际条数
 * @retval >=0 条数
 * @note meta = type | cpu<<16 | flags<<24
 * @warning 无
 * @attention ❌ ISR 慎用；❌ 不阻塞
 */
uint32_t cgrtos_trace_export(uint32_t *out, uint32_t max);
/** @brief UART 打印 Trace 摘要 @attention ❌ ISR；✅ 可能阻塞 */
void cgrtos_trace_dump(void);

#if CONFIG_USE_TRACE
#define CGRTOS_TRACE(type, a0, a1) cgrtos_trace_event((uint16_t)(type), (uint32_t)(a0), (uint32_t)(a1))
#else
#define CGRTOS_TRACE(type, a0, a1) ((void)0)
#endif

/** @brief 对象池占用快照 */
typedef struct {
    uint32_t tasks_used, tasks_max;
    uint32_t sem_used, sem_max;
    uint32_t mutex_used, mutex_max;
    uint32_t queue_used, queue_max;
    uint32_t event_used, event_max;
    uint32_t timer_used, timer_max;
} cgrtos_objects_stats_t;

typedef struct {
    cgrtos_sem_t *handle;
    int32_t       count;
    int32_t       max;
    uint32_t      waiters;
} cgrtos_sem_info_t;

typedef struct {
    cgrtos_mutex_t *handle;
    task_id_t       owner_id;
    uint32_t        recursive;
    uint32_t        waiters;
} cgrtos_mutex_info_t;

typedef struct {
    cgrtos_queue_t *handle;
    uint32_t        len;
    uint32_t        item_sz;
    uint32_t        count;
    uint32_t        wait_send;
    uint32_t        wait_recv;
} cgrtos_queue_info_t;

/** @brief 填充对象池统计 @return 0/-1 @attention ❌ ISR；❌ 不阻塞 */
int cgrtos_objects_stats_get(cgrtos_objects_stats_t *out);
/** @brief 导出信号量列表 @return 条数 @attention ❌ ISR；❌ 不阻塞 */
uint32_t cgrtos_sem_list_export(cgrtos_sem_info_t *out, uint32_t max);
/** @brief 导出互斥量列表 @return 条数 @attention ❌ ISR；❌ 不阻塞 */
uint32_t cgrtos_mutex_list_export(cgrtos_mutex_info_t *out, uint32_t max);
/** @brief 导出队列列表 @return 条数 @attention ❌ ISR；❌ 不阻塞 */
uint32_t cgrtos_queue_list_export(cgrtos_queue_info_t *out, uint32_t max);
/** @brief UART 打印对象池占用 @attention ❌ ISR；✅ 可能阻塞 */
void cgrtos_objects_dump(void);
/** @brief 已占用软定时器数 @return 数量 @attention ❌ ISR；❌ 不阻塞 */
uint32_t cgrtos_timer_count_used(void);

/**
 * @brief 打印运行时统计到 UART
 * @details 输出 uptime、任务数、切换、堆水位、LB 计数等。
 * @return 无
 * @retval 无
 * @note 诊断/调试命令用
 * @warning UART 输出可能阻塞
 * @attention ❌ ISR；✅ 可能阻塞（UART）
 */
void cgrtos_stats_dump(void);
/**
 * @brief 填充运行时统计快照
 * @details 复制 g_ticks、g_cs_count、堆统计等到 out。
 * @param[out] out 输出结构；NULL 则忽略
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无锁快照
 * @attention ✅ 只读快照；❌ 不阻塞
 */
void cgrtos_stats_get(cgrtos_runtime_stats_t *out);
#if CONFIG_SCHED_STATS
/**
 * @brief 查询单任务调度延迟统计
 * @details 填充 max/last/sum/samples/exec/cpu_util 等。
 * @param[in]  id  任务 ID
 * @param[out] out 输出结构；不可为 NULL
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 任务不存在或 out 为空
 * @note 须 CONFIG_SCHED_STATS=1
 * @warning 无
 * @attention ❌ ISR（持锁读）；❌ 不阻塞
 */
int cgrtos_task_get_sched_stats(task_id_t id, cgrtos_task_sched_stats_t *out);
/**
 * @brief 读取全局调度延迟峰值与采样数
 * @details 输出全局 max_sched_latency 与 sched_latency_samples。
 * @param[out] max_latency_global 可选；峰值 tick
 * @param[out] samples             可选；采样次数
 * @return 无
 * @retval 无
 * @note 指针可为 NULL 表示忽略
 * @warning 无
 * @attention ✅ 只读；❌ 不阻塞
 */
void cgrtos_sched_stats_get(tick_t *max_latency_global, uint32_t *samples);
/**
 * @brief 重置全局与 per-task 调度延迟统计
 * @details 清零 max/sum/samples 等计数。
 * @return 无
 * @retval 无
 * @note 诊断/测试用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_sched_stats_reset(void);
#endif

/**
 * @brief 查询任务累计运行 tick（TCB.exec）
 * @details 返回调度记账的执行 tick 累计值。
 * @param[in] id 任务 ID
 * @return 累计运行 tick
 * @retval ≥0 有效累计（无效任务通常为 0）
 * @note 无锁快照，SMP 下可能略有滞后
 * @warning 非墙钟时间；受 tick 粒度限制
 * @attention ✅ ISR；❌ 不阻塞
 */
tick_t cgrtos_task_get_runtime(task_id_t id);
/**
 * @brief 检查任务栈金丝雀是否完好
 * @details 校验栈底/填充模式；CONFIG_CHECK_STACK_OVERFLOW 关闭时可能恒成功。
 * @param[in] id 任务 ID
 * @return 结果码
 * @retval pdPASS 完好或未启用检测
 * @retval pdFAIL 检测到溢出或无效任务
 * @note 可在 tick/切换路径调用
 * @warning 发现溢出后应立即停止依赖该栈的逻辑
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_task_check_stack(task_id_t id);
/**
 * @brief 处理栈溢出（计数、钩子、默认 assert halt）
 * @details 更新统计、调用栈溢出钩子；默认路径随后 assert/_deadloop。
 * @param[in] task 溢出任务 TCB；不可为 NULL
 * @return 无（通常不返回）
 * @retval 无
 * @note 由调度器切换 / tick 抽检路径调用
 * @warning 钩子返回后仍可能 halt
 * @attention ✅ ISR；❌ 不阻塞（随后可能永久挂起）
 */
void cgrtos_task_handle_stack_overflow(cgrtos_task_t *task);
/**
 * @brief 切换离开已删除任务时回收 TCB 槽
 * @details 仅当该 TCB 不再是任意核的 g_current 时清零 id 并释放槽。
 * @param[in] task 待回收 TCB；可为刚切走的 DELETED 任务
 * @return 无
 * @retval 无
 * @note 调度器内部调用
 * @warning 应用勿直接调用
 * @attention ❌ 任务上下文（调度路径）；❌ 不阻塞
 */
void cgrtos_task_reclaim_deleted(cgrtos_task_t *task);
/**
 * @brief 主动让出 CPU（任务上下文 ecall 进入 trap 切换）
 * @details 置 g_yield_pending 与 g_force_yield；非 ISR 时 ecall 进入 switch_from_trap。
 * @return 无
 * @retval 无
 * @note 调度挂起或未运行时为空操作
 * @warning ISR 中请用 cgrtos_sched_yield_from_isr
 * @attention ❌ ISR；✅ 引起切换
 */
void cgrtos_sched_yield(void);
/**
 * @brief ISR 中请求重新调度
 * @details 置 g_yield_pending[cpu]=1，由 trap 返回路径 switch_from_trap；
 *          通常由 cgrtos_isr_notify_woken(NULL,1) 或 portYIELD_FROM_ISR 触发。
 * @return 无
 * @retval 无
 * @note 不直接 ecall；切换在 ISR 返回时完成
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_sched_yield_from_isr(void);
/**
 * @brief Trap 出口选下一个任务并返回其栈指针
 * @param[in] sp 当前 trap 帧栈指针
 * @return 下一任务应恢复的 sp；未切换时返回原 sp
 * @retval 非 NULL 下一任务栈指针
 * @note 由 trap 向量内部调用；处理 PRIORITY 粘性、CFS vruntime、idle 回退
 * @warning 禁止从任务上下文直接调用
 * @attention ✅ ISR/trap；❌ 不阻塞（可能切换）
 */
uint64_t *cgrtos_sched_switch_from_trap(uint64_t *sp);
/**
 * @brief 挂起调度器（嵌套计数）
 * @details g_sched_suspend_count++；挂起期间不进行上下文切换。
 * @return 无
 * @retval 无
 * @note 与 cgrtos_sched_resume 成对；可嵌套
 * @warning 挂起期间 delay/IPC 阻塞不会真正切换
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_sched_suspend(void);
/**
 * @brief 恢复调度器；嵌套归零时触发 yield
 * @details g_sched_suspend_count--；归零时调用 cgrtos_sched_yield 处理 pending 切换。
 * @return 无
 * @retval 无
 * @note 与 cgrtos_sched_suspend 成对
 * @warning 无
 * @attention ❌ ISR；✅ 可能引起切换
 */
void cgrtos_sched_resume(void);
/**
 * @brief 加权 Push 负载均衡
 * @details 比较两核加权负载，将 busy 核上最低紧急度 READY 任务 push 到 idle 核。
 * @return 无
 * @retval 无
 * @note 通常由 hart0 tick 周期调用
 * @warning SMP 下持 ready_lock；勿在持锁上下文重入
 * @attention ✅ ISR（tick 路径）；❌ 不阻塞
 */
void cgrtos_sched_load_balance(void);
/**
 * @brief Idle 工作窃取
 * @details 本核无 READY 且对端就绪数 ≥ CONFIG_LB_STEAL_MIN 时迁移 victim 任务。
 * @return 无
 * @retval 无
 * @note 受 CONFIG_SMP_IDLE_STEAL 控制
 * @warning 本地已有工作或 peer 不足时不窃取
 * @attention ❌ ISR（idle 上下文）；❌ 不阻塞
 */
void cgrtos_sched_idle_steal(void);
/**
 * @brief 统计某核 READY 任务数
 * @details 遍历该核就绪结构计数。
 * @param[in] cpu 逻辑核号
 * @return READY 任务数
 * @retval >=0 计数
 * @note 诊断/LB 用
 * @warning 无
 * @attention ❌ ISR（持 ready_lock）；❌ 不阻塞
 */
uint32_t cgrtos_sched_ready_count(uint8_t cpu);
/**
 * @brief 计算某核加权负载
 * @details 综合 READY 数、优先级权重、CFS 权重等。
 * @param[in] cpu 逻辑核号
 * @return 加权负载值
 * @retval >=0 负载分
 * @note LB 决策用
 * @warning 无
 * @attention ❌ ISR（持锁读）；❌ 不阻塞
 */
uint32_t cgrtos_sched_core_load(uint8_t cpu);
/**
 * @brief 返回当前最轻载核号
 * @details 比较各在线核 cgrtos_sched_core_load。
 * @return 最轻载核逻辑号
 * @retval 0..CONFIG_NUM_CORES-1 有效核
 * @note 新任务初始放置用
 * @warning 次核未 online 时可能回退 hart0
 * @attention ❌ ISR；❌ 不阻塞
 */
uint8_t cgrtos_sched_least_loaded_core(void);
/**
 * @brief 重新发布 boot 魔数并等待次核就绪
 * @details 写 g_boot_sync 并轮询 g_hart_stage[1]>=4；SMP 启动/recovery 用。
 * @return 无
 * @retval 无
 * @note 仅 hart0 在启动路径调用
 * @warning 轮询等待可能阻塞启动线程
 * @attention ❌ ISR；✅ 可能阻塞（轮询）
 */
void cgrtos_smp_kick_secondaries(void);
/**
 * @brief 将 EDF 任务下次释放挂入 release wheel
 * @details 按 deadline-period 计算下次释放 tick 并入轮槽。
 * @param[in] task 目标 TCB；须 SCHED_EDF 且 period>0
 * @return 无
 * @retval 无
 * @note CONFIG_USE_EDF=0 时为空操作
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞（持 ready_lock）
 */
void cgrtos_sched_edf_arm(cgrtos_task_t *task);
/**
 * @brief 向目标核发送软件 IPI（兼容 → hal_ipi_send）
 * @details 写 CLINT MSIP 触发次核 ipi handler；用于 yield/远程 tick/EDF kick。
 * @param[in] core 目标逻辑核号
 * @return 无
 * @retval 无
 * @note core=0 通常无操作
 * @warning 频繁 IPI 增加开销
 * @attention ✅ 任务/ISR 均可；❌ 不阻塞
 */
void cgrtos_smp_send_ipi(uint8_t core);
/**
 * @brief 系统 tick 中断处理（hart0 全局 + 本核切片）
 * @details 递增 g_ticks、处理 delayed/EDF release/软定时器/LB/时间片；hart0 发远程 tick IPI。
 * @return 无
 * @retval 无
 * @note 由 riscv_handle_timer → cgrtos_tick_handler 调用
 * @warning 执行时间影响全局节拍
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_tick_handler(void);
/**
 * @brief 本核时间片记账
 * @details 递减当前任务 slice_remain；用尽则置 yield_pending。
 * @return 无
 * @retval 无
 * @note 供次核 IPI 远程 tick 调用
 * @warning 无
 * @attention ✅ ISR/IPI 路径；❌ 不阻塞
 */
void cgrtos_tick_local(void);
/**
 * @brief ISR 嵌套计数 +1
 * @details 标记进入 ISR 上下文；配合 cgrtos_in_isr。
 * @return 无
 * @retval 无
 * @note trap 入口调用
 * @warning 须与 cgrtos_isr_exit 成对
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_isr_enter(void);
/**
 * @brief ISR 嵌套计数 -1
 * @details 标记离开 ISR 上下文。
 * @return 无
 * @retval 无
 * @note trap 出口调用
 * @warning 嵌套不匹配会破坏 in_isr 判定
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_isr_exit(void);

/**
 * @brief 自旋加锁
 * @details lock=0 则 CAS 置 1，否则忙等。
 * @param[in] lock 自旋锁变量；不可为 NULL
 * @return 无
 * @retval 无
 * @note 持锁区须极短；禁止阻塞 API
 * @warning 持锁顺序错误可能死锁
 * @attention ✅ 任务/ISR（须正确关中断）；✅ 可能自旋
 */
void cgrtos_spin_lock(spinlock_t *lock);
/**
 * @brief 自旋解锁
 * @details 将 *lock 置 0。
 * @param[in] lock 先前加锁的 spinlock
 * @return 无
 * @retval 无
 * @note 须与 spin_lock 成对且同核
 * @warning 未持锁 unlock 可能导致竞态
 * @attention ✅ 任务/ISR；❌ 不阻塞
 */
void cgrtos_spin_unlock(spinlock_t *lock);
/**
 * @brief 保存 mstatus 并清 MIE
 * @details 返回旧 mstatus 供 irq_restore 恢复。
 * @return 旧 mstatus 值
 * @retval 完整 mstatus 位图
 * @note 比 enter_critical 轻量，不持 g_klock
 * @warning 须与 cgrtos_irq_restore 成对
 * @attention ✅ 任务/ISR；❌ 不阻塞
 */
uint64_t cgrtos_irq_save(void);
/**
 * @brief 恢复 mstatus
 * @details 写回 flags 恢复 MIE 等位。
 * @param[in] flags cgrtos_irq_save 返回值
 * @return 无
 * @retval 无
 * @note 须与 irq_save 成对
 * @warning 无
 * @attention ✅ 任务/ISR；❌ 不阻塞
 */
void cgrtos_irq_restore(uint64_t flags);
/**
 * @brief 进入可嵌套临界区（关 IRQ + g_klock）
 * @details 嵌套计数递增；首次进入关 MIE 并 spin_lock(g_klock)。
 * @return 无
 * @retval 无
 * @note 与 cgrtos_exit_critical 成对；可嵌套
 * @warning 临界区内禁止阻塞 API
 * @attention ❌ ISR（任务侧 API）；❌ 不阻塞
 */
void cgrtos_enter_critical(void);
/**
 * @brief 退出临界区
 * @details 嵌套递减；最外层释放 g_klock 并恢复 MIE，可能 flush EDF kick。
 * @return 无
 * @retval 无
 * @note 最外层 exit 可能触发 IPI
 * @warning 嵌套不匹配危险
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_exit_critical(void);
/**
 * @brief 本核是否在临界区内
 * @details 查询 per-CPU 嵌套计数 >0。
 * @return 非 0 表示在临界区
 * @retval 1 在临界区
 * @retval 0 不在
 * @note 诊断/assert 用
 * @warning 无
 * @attention ✅ 任意上下文；❌ 不阻塞
 */
int cgrtos_in_critical(void);
/**
 * @brief 是否在 ISR 上下文
 * @details 查询 ISR 嵌套计数 >0。
 * @return 非 0 表示在 ISR
 * @retval 1 在 ISR
 * @retval 0 任务上下文
 * @note CONFIG_ISR_API_GUARD 依赖此判定
 * @warning 无
 * @attention ✅ 任意上下文；❌ 不阻塞
 */
int cgrtos_in_isr(void);
/**
 * @brief ISR 内临界区：抬高 PLIC threshold 至当前 syscall_max_prio
 * @details 屏蔽优先级 ≤ syscall_max 的中断；更高优先级仍可嵌套（不得调用 FromISR）。
 * @return 进入前的 PLIC threshold
 * @retval 旧 threshold，须交给 exit_critical_from_isr 恢复
 * @note 与任务侧 enter_critical（关 MIE + g_klock）不同，本 API 只改 PLIC 阈值
 * @warning 更高优先级 ISR 仍可能嵌套
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t cgrtos_enter_critical_from_isr(void);
/**
 * @brief 恢复 ISR 临界区前的 PLIC threshold
 * @details 写回 saved_threshold。
 * @param[in] saved_threshold enter_critical_from_isr 的返回值
 * @return 无
 * @retval 无
 * @note 须与 enter_critical_from_isr 成对
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_exit_critical_from_isr(uint32_t saved_threshold);
/**
 * @brief FromISR 统一唤醒通知
 * @details woken 非空则 need_yield 时置 pdTRUE；空则自动 yield_from_isr。
 * @param[out] woken      可选 woken 标志；可为 NULL
 * @param[in]  need_yield 非 0 表示应请求调度
 * @return 无
 * @retval 无
 * @note portYIELD_FROM_ISR 底层依赖
 * @warning woken 为 NULL 且 need_yield 会立即 yield_from_isr
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_isr_notify_woken(BaseType_t *woken, int need_yield);

/**
 * @brief 体系结构早期初始化（兼容 → hal_cpu_init）
 * @details 配置 CSR、中断向量等 CPU 级设置。
 * @return 无
 * @retval 无
 * @note 启动早期调用，早于调度器
 * @warning 重复调用行为未定义
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_arch_init(void);
/**
 * @brief 初始化 UART 控制台（兼容 → hal_console_init）
 * @details 配置波特率、TX/RX FIFO 等。
 * @return 无
 * @retval 无
 * @note cgrtos_init 路径调用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_uart_init(void);
/**
 * @brief 输出一字符（兼容 → hal_console_putc）
 * @details 写入 UART TX；FIFO 满时可能轮询等待。
 * @param[in] c 字符
 * @return 无
 * @retval 无
 * @note 无
 * @warning 可能阻塞 TX
 * @attention ❌ ISR 慎用；✅ 可能阻塞（TX FIFO）
 */
void cgrtos_uart_putc(char c);
/**
 * @brief 阻塞读一字符（兼容 → hal_console_getc）
 * @details 轮询直到 RX 有数据。
 * @return 读到的字符
 * @retval 0..255 数据字节
 * @note 无
 * @warning 永久阻塞直到有输入
 * @attention ❌ ISR；✅ 阻塞
 */
char cgrtos_uart_getc(void);
/**
 * @brief 非阻塞读一字符（兼容 → hal_console_pollc）
 * @details 立即返回 RX 状态。
 * @return 数据或 -1
 * @retval 0..255 有数据
 * @retval -1     RX 空
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int cgrtos_uart_pollc(void);
/**
 * @brief 输出字符串（兼容 → hal_console_puts）
 * @details 逐字符 putc 直到 NUL。
 * @param[in] s 以 NUL 结尾的字符串；NULL 忽略
 * @return 无
 * @retval 无
 * @note 无
 * @warning 可能阻塞 TX
 * @attention ❌ ISR 慎用；✅ 可能阻塞（UART）
 */
void cgrtos_uart_puts(const char *s);
/**
 * @brief 简易格式化打印（兼容 HAL printf）
 * @details 支持 s/d/u/x/p/c 及长整型变体；经 UART 输出。
 * @param[in] fmt 格式串
 * @param[in] ... 可变参数
 * @return 无
 * @retval 无
 * @note 无堆分配；缓冲区固定
 * @warning 可能阻塞 TX；非 ISR 安全
 * @attention ❌ ISR；✅ 可能阻塞（UART）
 */
void cgrtos_printf(const char *fmt, ...);
/**
 * @brief 初始化本核 mtimecmp 与 MTIE（兼容 → hal_timer_init）
 * @details 按 rate 配置 tick 中断周期。
 * @param[in] rate 系统 tick 频率 (Hz)
 * @return 无
 * @retval 无
 * @note 每 hart 独立调用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_clint_init(tick_t rate);
/**
 * @brief 读 SysTimer mtime（兼容 → hal_mtime_read）
 * @details 读取 CLINT mtime 寄存器。
 * @return 当前 mtime 值
 * @retval >=0 周期计数
 * @note 用于 cgrtos_delay_us 等
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
uint64_t cgrtos_mtime_read(void);
/**
 * @brief 初始化本 hart PLIC（兼容 → hal_irqc_init）
 * @details 配置 threshold、使能 MEIE；首次调用 cgrtos_irq_init。
 * @return 无
 * @retval 无
 * @note 每 hart 启动时调用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_plic_init(void);
/**
 * @brief PLIC claim（兼容 → hal_irqc_claim）
 * @details 读取并 claim 本 hart 最高优先级 pending 中断。
 * @return 中断源 ID
 * @retval >0 源编号
 * @retval 0  无待处理
 * @note 须与 complete 成对
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t cgrtos_plic_claim(void);
/**
 * @brief PLIC complete（兼容 → hal_irqc_complete）
 * @details 通知 PLIC 该中断处理完毕。
 * @param[in] irq 先前 claim 返回值
 * @return 无
 * @retval 无
 * @note 须与 claim 成对
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_plic_complete(uint32_t irq);
/**
 * @brief 设置本 hart PLIC 优先级阈值（兼容 → hal_irqc_set_threshold）
 * @details threshold 以下优先级的中断被屏蔽。
 * @param[in] threshold 新阈值；0=允许全部（priority>0）
 * @return 无
 * @retval 无
 * @note enter_critical_from_isr 使用
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_plic_set_threshold(uint32_t threshold);
/**
 * @brief 读取本 hart PLIC 优先级阈值（兼容 → hal_irqc_get_threshold）
 * @return 当前 threshold
 * @retval >=0 阈值
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t cgrtos_plic_get_threshold(void);
/**
 * @brief 设置中断源优先级（兼容 → hal_irqc_set_priority）
 * @param[in] irq      1..CONFIG_IRQ_MAX_SOURCES
 * @param[in] priority 0=禁用该源；1..CONFIG_IRQ_PRIORITY_MAX
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_plic_set_priority(uint32_t irq, uint32_t priority);
/**
 * @brief 读取中断源优先级（兼容 → hal_irqc_get_priority）
 * @param[in] irq 源编号
 * @return 优先级
 * @retval 0  非法 irq 或禁用
 * @retval 1..CONFIG_IRQ_PRIORITY_MAX 有效
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t cgrtos_plic_get_priority(uint32_t irq);
/**
 * @brief 对本 hart 使能指定中断源（兼容 → hal_irqc_enable）
 * @param[in] irq 源编号
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_plic_enable(uint32_t irq);
/**
 * @brief 对本 hart 禁用指定中断源（兼容 → hal_irqc_disable）
 * @param[in] irq 源编号
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_plic_disable(uint32_t irq);

/**
 * @defgroup cgrtos_irq 中断优先级分组与处理注册
 * @{
 */
/**
 * @brief 初始化 IRQ 子系统（清零 handler 表）
 * @details 由 cgrtos_plic_init 首次调用。
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_irq_init(void);
/**
 * @brief 配置中断源优先级并可选使能
 * @param[in] irq      中断源编号（1..CONFIG_IRQ_MAX_SOURCES）
 * @param[in] priority 0..CONFIG_IRQ_PRIORITY_MAX；0 禁用该源
 * @param[in] enable   非 0 则使能该源，否则禁用
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法
 * @note 封装 plic_set_priority + enable/disable
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_irq_configure(uint32_t irq, uint32_t priority, int enable);
/**
 * @brief 注册 PLIC 处理函数
 * @details claim 后由 cgrtos_irq_dispatch 调用 handler(irq, arg)。
 * @param[in] irq     源编号
 * @param[in] handler 回调；须短小、非阻塞
 * @param[in] arg     透传参数
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 参数非法或表满
 * @note 仅当该源优先级 ≤ syscall_max 时 handler 内可调用 FromISR
 * @warning handler 阻塞会延迟其他中断
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_irq_register(uint32_t irq, cgrtos_irq_handler_t handler, void *arg);
/**
 * @brief 注销处理函数（不自动 disable 源）
 * @param[in] irq 源编号
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 未注册或非法
 * @note 无
 * @warning 注销后 claim 仍可能发生但无 handler
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_irq_unregister(uint32_t irq);
/**
 * @brief 查询已注册 handler（测试/诊断）
 * @param[in] irq 源编号
 * @return handler 指针
 * @retval 非 NULL 已注册
 * @retval NULL    未注册或非法 irq
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
cgrtos_irq_handler_t cgrtos_irq_get_handler(uint32_t irq);
/**
 * @brief 设置允许调用 FromISR 的最高优先级上界
 * @details enter_critical_from_isr 用此值作 PLIC threshold。
 * @param[in] max_prio 新上界（超过 CONFIG_IRQ_PRIORITY_MAX 时钳制）
 * @return 无
 * @retval 无
 * @note 默认 CONFIG_IRQ_SYSCALL_MAX_PRIO
 * @warning 上界过高可能使 FromISR 与嵌套 ISR 冲突
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_irq_set_syscall_max_priority(uint32_t max_prio);
/**
 * @brief 当前 FromISR 允许的最高优先级
 * @return 运行时 syscall_max_prio
 * @retval 0..CONFIG_IRQ_PRIORITY_MAX
 * @note 无
 * @warning 无
 * @attention ✅ 只读；❌ 不阻塞
 */
uint32_t cgrtos_irq_get_syscall_max_priority(void);
/**
 * @brief 分发已 claim 的外部中断
 * @details 查找 handler 并调用；无 handler 时空操作。
 * @param[in] irq claim 返回值
 * @return 无
 * @retval 无
 * @note 供 riscv_handle_external 调用
 * @warning handler 须短小非阻塞
 * @attention ✅ ISR；❌ 不阻塞
 */
void cgrtos_irq_dispatch(uint32_t irq);
/** @} */
/**
 * @brief 原子读取 g_ticks
 * @details 返回全局系统节拍计数。
 * @return 当前 tick
 * @retval >=0 g_ticks 快照
 * @note SMP 下 hart0 递增；读可能略有撕裂
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
tick_t cgrtos_get_ticks(void);
/**
 * @brief 有界 snprintf 风格格式化
 * @details 写入 buf，最多 n-1 字符 + NUL；不支持浮点。
 * @param[out] buf 输出缓冲
 * @param[in]  n   缓冲容量（含 NUL）
 * @param[in]  fmt 格式串
 * @param[in]  ... 可变参数
 * @return 写入字符数（不含 NUL）
 * @retval >=0 成功长度
 * @retval -1  参数非法
 * @note 无堆分配
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int cgrtos_snprintf(char *buf, unsigned long n, const char *fmt, ...);

/**
 * @brief 断言失败处理（打印后 halt）
 * @details 递增 g_assert_count；可选 assert_hook；打印 [ASSERT FAILED] 后 WFI 死循环。
 * @param[in] file 源文件名
 * @param[in] line 行号
 * @return 无（不返回）
 * @retval 无
 * @note 由 configASSERT/CGRTOS_ASSERT 调用
 * @warning 调用后系统永不返回
 * @attention ✅ ISR；✅ 可能阻塞（halt 前 UART 输出）
 */
void cgrtos_assert_failed(const char *file, int line);

/* --- Scheduler internals（IPC / task 使用） --- */

/**
 * @brief 任务应入队的目标核（亲和性解析）
 * @details 解析 cpu_aff/run_cpu；0xFF 选最轻载核。
 * @param[in] task 目标 TCB；不可为 NULL
 * @return 逻辑核号
 * @retval 0..CONFIG_NUM_CORES-1 目标核
 * @note IPC/调度内部使用
 * @warning 无
 * @attention ❌ ISR（持锁）；❌ 不阻塞
 */
uint8_t cgrtos_sched_target_core(cgrtos_task_t *task);
/**
 * @brief 按策略插入就绪结构
 * @details 根据 policy 入 PRIORITY/RR/CFS/EDF 等就绪队列；可能 IPI。
 * @param[in] task 任务 TCB；须 READY 态
 * @return 无
 * @retval 无
 * @note 调用方通常已持 g_klock
 * @warning 重复 add 会导致链表损坏
 * @attention ❌ ISR；❌ 不阻塞（可能 IPI）
 */
void cgrtos_sched_add_ready(cgrtos_task_t *task);
/**
 * @brief 刷新推迟的 MC-EDF 踢核
 * @details 持 g_klock 期间 EDF 入队只置 pending，出锁后再 IPI，避免死锁。
 * @return 无
 * @retval 无
 * @note 由 exit_critical 最外层调用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞（可能 IPI）
 */
void cgrtos_sched_edf_kick_flush(void);
/**
 * @brief 从就绪结构移除
 * @details 按 policy 从对应就绪队列摘除 task。
 * @param[in] task 任务 TCB
 * @return 无
 * @retval 无
 * @note 调用方通常已持 g_klock
 * @warning 不在就绪态时可能空操作
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_sched_remove_ready(cgrtos_task_t *task);
/**
 * @brief 阻塞当前任务（相对超时）
 * @details 置 BLOCKED、入 delayed/wait 结构；timeout 到期或 signal 唤醒。
 * @param[in] task    目标 TCB（通常为 current）
 * @param[in] reason  阻塞原因
 * @param[in] obj     关联 IPC 对象；可为 NULL
 * @param[in] timeout 0=立即；portMAX_DELAY=无限；否则相对 tick
 * @return 无
 * @retval 无
 * @note 调用后须 yield 才真正切换
 * @warning ISR 中禁止
 * @attention ❌ ISR；✅ 阻塞并切换
 */
void cgrtos_sched_block(cgrtos_task_t *task, block_reason_t reason,
                        void *obj, tick_t timeout);
/**
 * @brief 阻塞到绝对 wake_tick
 * @details 以 BLOCK_DELAY 入 delayed 链；wake_tick 为绝对 g_ticks。
 * @param[in] task      目标 TCB
 * @param[in] reason    阻塞原因
 * @param[in] obj       关联对象；可为 NULL
 * @param[in] wake_tick 绝对唤醒 tick；须 > g_ticks
 * @return 无
 * @retval 无
 * @note 调用前应保证 wake_tick > g_ticks
 * @warning ISR 中禁止
 * @attention ❌ ISR；✅ 阻塞并切换
 */
void cgrtos_sched_block_until(cgrtos_task_t *task, block_reason_t reason,
                              void *obj, tick_t wake_tick);
/**
 * @brief 解除阻塞并可选跨核 IPI
 * @details 置 READY、add_ready；目标在他核时 send_ipi。
 * @param[in] task 被唤醒 TCB
 * @return 结果码
 * @retval pdPASS 成功
 * @retval pdFAIL 非 BLOCKED 或无效
 * @note IPC give/signal 路径调用
 * @warning 无
 * @attention ❌ ISR（任务路径）；✅ 可能切换/IPI
 */
int cgrtos_sched_unblock(cgrtos_task_t *task);
/**
 * @brief 等待队列 FIFO 入队
 * @details 将 task 链到 *head 尾部。
 * @param[in,out] head 等待队列头指针
 * @param[in]     task 等待任务 TCB
 * @return 无
 * @retval 无
 * @note 调用方须持对象锁或 g_klock
 * @warning 重复入队损坏链表
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_wait_list_add(cgrtos_task_t *volatile *head, cgrtos_task_t *task);
/**
 * @brief 等待队列按优先级插入
 * @details 按 prio 降序插入 *head 链表。
 * @param[in,out] head 等待队列头指针
 * @param[in]     task 等待任务 TCB
 * @return 无
 * @retval 无
 * @note 互斥量/信号量等待用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_wait_list_add_priority(cgrtos_task_t *volatile *head, cgrtos_task_t *task);
/**
 * @brief 弹出最高优先级等待者
 * @details 从 *head 摘最高 prio 任务并返回。
 * @param[in,out] head 等待队列头指针
 * @return 弹出的 TCB
 * @retval 非 NULL 有等待者
 * @retval NULL    队列为空
 * @note give/unlock 路径调用
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
cgrtos_task_t *cgrtos_wait_list_pop_highest(cgrtos_task_t *volatile *head);
/**
 * @brief 从等待队列摘除指定任务
 * @details 从 *head 链表中移除 task 节点。
 * @param[in,out] head 等待队列头指针
 * @param[in]     task 待摘除 TCB
 * @return 无
 * @retval 无
 * @note 超时/删除/purge 路径调用
 * @warning task 不在队列上时可能空操作
 * @attention ❌ ISR；❌ 不阻塞
 */
void cgrtos_wait_list_remove(cgrtos_task_t *volatile *head, cgrtos_task_t *task);
/**
 * @brief 删除/超时前清理任务在各 IPC 等待链上的节点
 * @details 从相关 wait_q / 延迟结构摘除 task，避免悬空链表节点。
 * @param[in] task 待清理任务 TCB；不可为 NULL
 * @return 无
 * @retval 无
 * @note 由 delete/timeout 路径调用；须在合适临界区内
 * @warning 遗漏清理会导致 IPC 链表损坏
 * @attention ❌ ISR（通常）；❌ 不阻塞
 */
void cgrtos_task_purge_waits(cgrtos_task_t *task);

/* -------------------------------------------------------------------------- */
/* FreeRTOS-style compatibility macros                                        */
/* -------------------------------------------------------------------------- */

/** @def portMAX_DELAY 无限阻塞超时值 */
#define portMAX_DELAY               ((tick_t)-1)
/** @def portMS_TO_TICK 毫秒转 tick（四舍五入） */
#define portMS_TO_TICK(ms)          ((((tick_t)(ms)) * CONFIG_TICK_RATE_HZ + 500) / 1000)
/** @def portTICK_PERIOD_MS 每 tick 对应的毫秒数 */
#define portTICK_PERIOD_MS          (1000 / CONFIG_TICK_RATE_HZ)
/** @def portUS_TO_MTIME 微秒转 mtime 周期数 */
#define portUS_TO_MTIME(us) \
    (((uint64_t)(us) * (CONFIG_TIMER_CLOCK_HZ)) / 1000000ULL)
/** @def portMTIME_PER_TICK 每个系统 tick 对应的 mtime 周期数 */
#define portMTIME_PER_TICK \
    (CONFIG_TIMER_CLOCK_HZ / (uint64_t)CONFIG_TICK_RATE_HZ)

/**
 * @def configASSERT
 * @brief FreeRTOS 风格运行时断言；失败则 cgrtos_assert_failed
 */
#define configASSERT(expr) \
    do { if (!(expr)) { cgrtos_assert_failed(__FILE__, __LINE__); } } while (0)
/** @def CGRTOS_ASSERT 同 configASSERT */
#define CGRTOS_ASSERT(expr) configASSERT(expr)
/** @def STACK_CANARY_VALUE 任务栈低地址金丝雀（与 task_init_stack 一致） */
#define STACK_CANARY_VALUE  0xDEADBEEFCAFE0000ULL

/** @def xTaskCreate FreeRTOS 兼容：创建 SCHED_PRIORITY 任务（stack 参数忽略，用固定 CONFIG_TASK_STACK_WORDS） */
#define xTaskCreate(fn, name, stack, arg, prio, handle) \
    ((void)(stack), (*(handle) = (void *)(uintptr_t)cgrtos_task_create(name, fn, arg, prio, SCHED_PRIORITY)), pdPASS)

/** @def vTaskDelete 删除任务 */
#define vTaskDelete(id)             cgrtos_task_delete((task_id_t)(uintptr_t)(id))
/** @def vTaskSuspend 挂起任务 */
#define vTaskSuspend(id)            cgrtos_task_suspend((task_id_t)(uintptr_t)(id))
/** @def vTaskResume 恢复任务 */
#define vTaskResume(id)             cgrtos_task_resume((task_id_t)(uintptr_t)(id))
/** @def vTaskPrioritySet 设置优先级 */
#define vTaskPrioritySet(id, p)     cgrtos_task_set_priority((task_id_t)(uintptr_t)(id), (uint8_t)(p))
/** @def eTaskGetState 查询任务状态 */
#define eTaskGetState(id)           cgrtos_task_get_state((task_id_t)(uintptr_t)(id))
/** @def uxTaskGetStackHighWaterMark 栈水位（未用字节） */
#define uxTaskGetStackHighWaterMark(h) cgrtos_task_get_stack_high_water_mark((task_id_t)(uintptr_t)(h))

/** @def vTaskDelay 相对延时（tick） */
#define vTaskDelay(t)               cgrtos_delay(t)
/** @def vTaskDelayUntil 绝对周期延时 */
#define vTaskDelayUntil(p, i)       cgrtos_delay_until(p, i)
/** @def vTaskYield 主动让出 CPU */
#define vTaskYield()                cgrtos_task_yield()
/** @def taskYIELD 同 vTaskYield */
#define taskYIELD()                 cgrtos_task_yield()

/** @def vTaskSuspendAll 挂起调度器 */
#define vTaskSuspendAll()           cgrtos_sched_suspend()
/** @def xTaskResumeAll 恢复调度器 */
#define xTaskResumeAll()            cgrtos_sched_resume()

/** @def xSemaphoreCreateCounting 计数信号量 */
#define xSemaphoreCreateCounting(max, init) cgrtos_sem_create(init, max)
/** @def xSemaphoreCreateBinary 二进制信号量 */
#define xSemaphoreCreateBinary()    cgrtos_sem_create_binary()
/** @def xSemaphoreTake 获取信号量 */
#define xSemaphoreTake(s, t)        cgrtos_sem_take(s, t)
/** @def xSemaphoreGive 释放信号量 */
#define xSemaphoreGive(s)           cgrtos_sem_give(s)
/** @def xSemaphoreGiveFromISR ISR 释放；hp 为 BaseType_t* woken（可为 NULL） */
#define xSemaphoreGiveFromISR(s, hp) cgrtos_sem_give_from_isr((s), (hp))
/** @def xSemaphoreTakeFromISR ISR 非阻塞获取；hp 忽略 */
#define xSemaphoreTakeFromISR(s, hp) \
    ((void)(hp), cgrtos_sem_take_from_isr(s))
/** @def vSemaphoreDelete 删除信号量 */
#define vSemaphoreDelete(s)         cgrtos_sem_delete(s)

/** @def xQueueCreate 创建消息队列 */
#define xQueueCreate(len, sz)       cgrtos_queue_create(len, sz)
/** @def xQueueSend 发送到队列 */
#define xQueueSend(q, d, t)         cgrtos_queue_send(q, d, t)
/** @def xQueueReceive 从队列接收 */
#define xQueueReceive(q, b, t)      cgrtos_queue_receive(q, b, t)
/** @def xQueueSendFromISR ISR 发送；hp 为 woken 指针 */
#define xQueueSendFromISR(q, d, hp) cgrtos_queue_send_from_isr((q), (d), (hp))
/** @def xQueueReceiveFromISR ISR 接收；hp 为 woken 指针 */
#define xQueueReceiveFromISR(q, b, hp) cgrtos_queue_receive_from_isr((q), (b), (hp))
/** @def uxQueueMessagesWaiting 队列当前消息数 */
#define uxQueueMessagesWaiting(q)   cgrtos_queue_messages_waiting(q)
/** @def vQueueDelete 删除队列 */
#define vQueueDelete(q)             cgrtos_queue_delete(q)

/** @def xEventGroupCreate 创建事件组 */
#define xEventGroupCreate()         cgrtos_event_group_create()
/** @def xEventGroupSetBits 置事件位 */
#define xEventGroupSetBits(eg, f)   cgrtos_event_group_set(eg, f)
/** @def xEventGroupSetBitsFromISR ISR 置位；hp 为 woken 指针 */
#define xEventGroupSetBitsFromISR(eg, f, hp) \
    cgrtos_event_group_set_from_isr((eg), (f), (hp))
/** @def xEventGroupClearBits 清事件位 */
#define xEventGroupClearBits(eg, f) cgrtos_event_group_clear(eg, f)
/** @def xEventGroupClearBitsFromISR ISR 清位（无 woken） */
#define xEventGroupClearBitsFromISR(eg, f) cgrtos_event_group_clear_from_isr((eg), (f))
/** @def xEventGroupWaitBits 等待事件位 */
#define xEventGroupWaitBits(eg, f, c, wa, t) \
    cgrtos_event_group_wait_bits(eg, f, c, wa, t)

/** @def pvPortMalloc TLSF 堆分配 */
#define pvPortMalloc(sz)            cgrtos_malloc(sz)
/** @def vPortFree 释放堆块 */
#define vPortFree(p)                cgrtos_free(p)
/** @def xPortGetFreeHeapSize 当前空闲堆字节 */
#define xPortGetFreeHeapSize()      cgrtos_get_free_heap()
/** @def xPortGetMinimumEverFreeHeapSize 历史最小空闲堆 */
#define xPortGetMinimumEverFreeHeapSize() cgrtos_get_min_free_heap()

/**
 * @def portYIELD_FROM_ISR
 * @brief ISR 末尾：若 hp（BaseType_t 值，非指针）非 pdFALSE 则请求调度
 */
#define portYIELD_FROM_ISR(hp) \
    do { if ((hp) != pdFALSE) { cgrtos_sched_yield_from_isr(); } } while (0)
/**
 * @def portSET_INTERRUPT_MASK_FROM_ISR
 * @brief 抬高 PLIC 阈值屏蔽 FromISR 级中断；返回值交给 CLEAR
 */
#define portSET_INTERRUPT_MASK_FROM_ISR() cgrtos_enter_critical_from_isr()
/**
 * @def portCLEAR_INTERRUPT_MASK_FROM_ISR
 * @brief 恢复 SET 返回的 PLIC 阈值
 */
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(m) cgrtos_exit_critical_from_isr(m)

/** @def vApplicationIdleHook 空钩子占位；请用 cgrtos_set_idle_hook */
#define vApplicationIdleHook()      /* app override via cgrtos_set_idle_hook */
/** @def vApplicationTickHook 空钩子占位；请用 cgrtos_set_tick_hook */
#define vApplicationTickHook()      /* app override via cgrtos_set_tick_hook */

/** @def read_csr 读 CSR，例如 read_csr(mhartid) */
#define read_csr(reg) ({ \
    uint64_t __v; \
    asm volatile("csrr %0, " #reg : "=r"(__v)); \
    __v; \
})

/** @def write_csr 写 CSR */
#define write_csr(reg, val) \
    asm volatile("csrw " #reg ", %0" :: "r"((uint64_t)(val)))

/** @def set_csr_bits 置位 CSR 中若干 bit */
#define set_csr_bits(reg, bits) ({ \
    uint64_t __v = read_csr(reg); \
    __v |= (bits); \
    write_csr(reg, __v); \
})

/** @def clear_csr_bits 清除 CSR 中若干 bit */
#define clear_csr_bits(reg, bits) ({ \
    uint64_t __v = read_csr(reg); \
    __v &= ~(bits); \
    write_csr(reg, __v); \
})

#endif /* CGRTOS_H */
