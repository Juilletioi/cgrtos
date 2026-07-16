#ifndef CGRTOS_H
#define CGRTOS_H

/**
 * @file cgrtos.h
 * @brief CG-RTOS 公共 API 头文件（FreeRTOS 风格命名、SMP 多策略内核）
 *
 * @details
 * 调度：PRIORITY / RR / CFS / EDF / HYBRID + 加权 SMP 负载均衡。\n
 * 启动：hart0 `main` → `cgrtos_init` / kick → 创建任务 → `cgrtos_start`；\n
 *       hart1 `secondary_main` → `cgrtos_start_secondary`。\n
 * 主流程与流程图见 `docs/ARCHITECTURE.md`。
 *
 * 典型用法：
 * @code
 *   cgrtos_init();
 *   cgrtos_task_create(...);
 *   cgrtos_start();
 * @endcode
 */

#include <stdint.h>
#include <stddef.h>
#include "list.h"

/* -------------------------------------------------------------------------- */
/* Version & configuration                                                     */
/* -------------------------------------------------------------------------- */

/** @def CGRTOS_VERSION 内核版本字符串 */
#define CGRTOS_VERSION              "4.3.0"

/** @def CONFIG_SMP_ENABLE 使能 SMP 代码路径（1=开） */
#define CONFIG_SMP_ENABLE           1
/** @def CONFIG_NUM_CORES 逻辑 hart / 核数量 */
#define CONFIG_NUM_CORES            2
/** @def CONFIG_MAX_TASKS 任务控制块池大小（含占用槽） */
#define CONFIG_MAX_TASKS            64
/** @def CONFIG_MAX_PRIORITY 最高优先级编号（0..本宏） */
#define CONFIG_MAX_PRIORITY         31
/** @def CONFIG_TICK_RATE_HZ 系统节拍频率 (Hz) */
#define CONFIG_TICK_RATE_HZ         1000
/** @def CONFIG_TIMER_CLOCK_HZ SysTimer / mtime 时钟频率 (Hz) */
#define CONFIG_TIMER_CLOCK_HZ       1000000ULL
/** @def CONFIG_TIME_SLICE_TICKS RR/部分策略默认时间片（tick） */
#define CONFIG_TIME_SLICE_TICKS     10
/** @def CONFIG_RT_PRIO_THRESHOLD Hybrid：>= 该优先级走 Priority 侧 */
#define CONFIG_RT_PRIO_THRESHOLD    20
/** @def CONFIG_CFS_SLICE_TICKS CFS 时间片（tick） */
#define CONFIG_CFS_SLICE_TICKS      5
/** @def CONFIG_TIMER_WHEEL_SLOTS 软定时器时间轮槽数（须为 2 的幂） */
#define CONFIG_TIMER_WHEEL_SLOTS    256
/** @def CONFIG_EDF_RELEASE_SLOTS EDF 下次释放轮槽数（须为 2 的幂） */
#define CONFIG_EDF_RELEASE_SLOTS    256
/** @def CONFIG_TASK_STACK_WORDS 普通任务内嵌栈深度（字，64-bit） */
#define CONFIG_TASK_STACK_WORDS     256
/** @def CONFIG_HEAP_SIZE TLSF 堆字节数 */
#define CONFIG_HEAP_SIZE            (256 * 1024)
/** @def CONFIG_IDLE_STACK_WORDS Idle 任务栈深度（字） */
#define CONFIG_IDLE_STACK_WORDS     128
/** @def CONFIG_TIMER_TASK_PRIO 软定时器守护任务优先级 */
#define CONFIG_TIMER_TASK_PRIO      (CONFIG_MAX_PRIORITY - 1)
/** @def CONFIG_TIMER_QUEUE_LEN 定时器命令队列深度 */
#define CONFIG_TIMER_QUEUE_LEN      16
/** @def CONFIG_USE_TIMERS 编译软定时器子系统 */
#define CONFIG_USE_TIMERS           1
/** @def CONFIG_USE_TASK_NOTIFICATIONS 编译任务通知 */
#define CONFIG_USE_TASK_NOTIFICATIONS 1
/** @def CONFIG_CHECK_STACK_OVERFLOW 栈金丝雀 / 高水位检测 */
#define CONFIG_CHECK_STACK_OVERFLOW 1
/**
 * @def CONFIG_STACK_CHECK_ON_TICK
 * @brief 每 tick 对本核当前任务做金丝雀抽检（需 CONFIG_CHECK_STACK_OVERFLOW）
 */
#define CONFIG_STACK_CHECK_ON_TICK  1
/**
 * @def CONFIG_MUTEX_MAX_RECURSIVE
 * @brief 互斥量递归加锁上限（额外层数；总持有次数 = recursive+1）
 */
#define CONFIG_MUTEX_MAX_RECURSIVE  64
/** @def CONFIG_USE_HOOKS 编译 idle/tick/malloc-fail 钩子 */
#define CONFIG_USE_HOOKS            1
/**
 * @def CONFIG_IDLE_BUSY_PUMP
 * @brief Idle 忙等推进 mtime（QEMU 防 WFI 卡死 SysTimer）
 * @note 真实硅片可改为 0，改用 WFI。
 */
#define CONFIG_IDLE_BUSY_PUMP       1
/** @def CONFIG_LOAD_BALANCE_HYST Push 均衡启动的最小加权负载差 */
#define CONFIG_LOAD_BALANCE_HYST    3
/** @def CONFIG_LOAD_BALANCE_PERIOD 两次 Push 尝试的间隔（tick） */
#define CONFIG_LOAD_BALANCE_PERIOD  16
/** @def CONFIG_LB_STEAL_MIN Steal：对端 READY 数达到此阈值才偷 */
#define CONFIG_LB_STEAL_MIN         2
/**
 * @def CONFIG_SMP_IDLE_STEAL
 * @brief 1=使能 Idle Steal；0=仅 Push（QEMU 默认更稳）
 */
#define CONFIG_SMP_IDLE_STEAL       0
/** @def CONFIG_LB_PRIO_SCALE 优先级任务权重系数：weight+=(prio+1)*scale */
#define CONFIG_LB_PRIO_SCALE        4
/** @def CONFIG_LB_CFS_WEIGHT CFS/Hybrid-CFS 基础权重 */
#define CONFIG_LB_CFS_WEIGHT        3
/** @def CONFIG_HEAP_POISON 堆喷毒（调试，0=关） */
#define CONFIG_HEAP_POISON          0
/** @def CONFIG_SMP_INITIAL_PLACE 新建任务放到当前最轻载核 */
#define CONFIG_SMP_INITIAL_PLACE    1

/** @def CGRTOS_TASK_NAME_MAX 任务名最大长度（含终结符预算） */
#define CGRTOS_TASK_NAME_MAX        16
/** @def CGRTOS_MAX_SEM 信号量对象池容量 */
#define CGRTOS_MAX_SEM              64
/** @def CGRTOS_MAX_MUTEX 互斥量对象池容量 */
#define CGRTOS_MAX_MUTEX            32
/** @def CGRTOS_MAX_QUEUE 队列对象池容量 */
#define CGRTOS_MAX_QUEUE            32
/** @def CGRTOS_MAX_TIMER 软定时器对象池容量 */
#define CGRTOS_MAX_TIMER            32
/** @def CGRTOS_MAX_EVENT 事件组对象池容量 */
#define CGRTOS_MAX_EVENT            16
/** @def CGRTOS_MAX_STREAM_BUFFER Stream/Message Buffer 池容量 */
#define CGRTOS_MAX_STREAM_BUFFER    8
/** @def CGRTOS_MAX_QUEUE_SET QueueSet 池容量 */
#define CGRTOS_MAX_QUEUE_SET        4
/** @def CGRTOS_QUEUE_SET_LENGTH 每个 QueueSet 最大成员数 */
#define CGRTOS_QUEUE_SET_LENGTH     8
/** @def CGRTOS_FS_MAX_INODES RAM FS inode 数 */
#define CGRTOS_FS_MAX_INODES        64
/** @def CGRTOS_FS_MAX_FD 打开文件描述符数 */
#define CGRTOS_FS_MAX_FD            16
/** @def CGRTOS_FS_MAX_NAME 目录项名最大长度（含 NUL） */
#define CGRTOS_FS_MAX_NAME          32
/** @def CGRTOS_FS_MAX_FILE_BYTES 单文件最大字节数 */
#define CGRTOS_FS_MAX_FILE_BYTES    (32 * 1024)

/** @def pdPASS 成功（FreeRTOS 兼容） */
#define pdPASS                      0
/** @def pdFAIL 失败 */
#define pdFAIL                      (-1)
/** @def pdTRUE 布尔真 */
#define pdTRUE                      1
/** @def pdFALSE 布尔假 */
#define pdFALSE                     0
/** @def errQUEUE_EMPTY 队列空 */
#define errQUEUE_EMPTY              (-1)
/** @def errQUEUE_FULL 队列满 */
#define errQUEUE_FULL               (-2)

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

/** @enum task_state_t 任务内部状态机 */
typedef enum {
    TASK_READY = 0,   /**< 就绪，位于就绪队列 */
    TASK_RUNNING,     /**< 正在某核上运行 */
    TASK_BLOCKED,     /**< 阻塞（延时/IPC/通知） */
    TASK_SUSPENDED,   /**< 被挂起 */
    TASK_DELETED      /**< 已删除，槽可复用 */
} task_state_t;

/** @enum sched_policy_t 调度策略 */
typedef enum {
    SCHED_RR = 0,     /**< 同优先级时间片轮转 */
    SCHED_PRIORITY,   /**< 纯优先级，同优先级粘滞 */
    SCHED_CFS,        /**< 完全公平调度（vruntime） */
    SCHED_EDF,        /**< 最早截止时间优先 */
    SCHED_HYBRID      /**< 高优先级 Priority，否则 CFS */
} sched_policy_t;

/** @enum block_reason_t 阻塞原因（配合 block_obj） */
typedef enum {
    BLOCK_NONE = 0,     /**< 未阻塞 */
    BLOCK_DELAY,        /**< 延时 */
    BLOCK_SEM,          /**< 信号量 */
    BLOCK_MUTEX,        /**< 互斥量 */
    BLOCK_QUEUE_SEND,   /**< 队列发送端 */
    BLOCK_QUEUE_RECV,   /**< 队列接收端 */
    BLOCK_EVENT,        /**< 事件组 */
    BLOCK_NOTIFY,       /**< 任务通知 */
    BLOCK_STREAM_SEND,  /**< StreamBuffer 发送 */
    BLOCK_STREAM_RECV,  /**< StreamBuffer 接收 */
    BLOCK_QUEUE_SET     /**< QueueSet select */
} block_reason_t;

/** @enum eTaskState_t FreeRTOS 风格对外任务状态 */
typedef enum {
    eReady = 0,
    eRunning,
    eBlocked,
    eSuspended,
    eDeleted,
    eInvalid
} eTaskState_t;

/** @enum eNotifyAction_t 任务通知合并动作 */
typedef enum {
    eNoAction = 0,              /**< 仅置 pending */
    eSetBits,                   /**< 按位或 */
    eIncrement,                 /**< 加一 */
    eSetValueWithOverwrite,     /**< 覆盖写入 */
    eSetValueWithoutOverwrite   /**< 仅当原值为 0 时写入 */
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
    uint8_t             base_prio;       /* priority before PI boost */
    task_state_t        state;
    sched_policy_t      policy;
    block_reason_t      block_reason;
    tick_t              wake_tick;       /* delayed-list absolute wake time */
    tick_t              deadline;        /* EDF absolute deadline */
    tick_t              period;          /* EDF period (0 = sporadic) */
    tick_t              exec;            /* ticks spent running (stats) */
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
} cgrtos_runtime_stats_t;

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
    uint8_t             inherit;   /**< 是否启用优先级继承 */
    uint8_t             in_use;    /**< 池槽占用标志 */
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
    cgrtos_timer_t     *timer; /**< 关联定时器 */
    uint8_t             cmd;   /**< 命令码，如 TIMER_CMD_EXPIRE */
} timer_cmd_t;

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
/** @brief 从核已进入调度循环 */
extern volatile uint32_t g_secondary_online;
/** @brief SMP 启动同步魔数（startup.S 轮询） */
extern volatile uint32_t g_boot_sync;

/* -------------------------------------------------------------------------- */
/* Architecture / startup                                                     */
/* -------------------------------------------------------------------------- */

/** @brief 保存 *cur 栈、切换到 nxt（兼容入口，通常走 ecall yield） */
extern void context_switch(uint64_t **cur, uint64_t *nxt);
/** @brief 装载首任务 trap 帧并 mret 进入任务上下文 */
extern void start_first_task(uint64_t *sp);
/** @brief 处理软件中断（MSIP / IPI） */
extern void riscv_handle_ipi(uint64_t *f);
/** @brief 处理定时器中断（MTIP） */
extern void riscv_handle_timer(uint64_t *f);
/** @brief 处理外部中断（PLIC） */
extern void riscv_handle_external(uint64_t *f);
/** @brief 处理同步异常 */
extern void riscv_handle_exception(uint64_t *f, uint64_t cause, uint64_t epc);

/** @brief 致命挂起循环（WFI） */
void _deadloop(void);
/**
 * @brief 应用入口（hart0）
 * @param hartid 核号
 * @param fdt 设备树（可忽略）
 * @param end 链接脚本 _end 等
 */
int main(int hartid, void *fdt, void *end);
/**
 * @brief 从核 C 入口
 * @param hartid 核号（通常为 1）
 */
void secondary_main(int hartid);

/* -------------------------------------------------------------------------- */
/* Kernel API                                                                 */
/* -------------------------------------------------------------------------- */

/** @brief 初始化内核全局状态、平台驱动并 kick 从核 */
void cgrtos_init(void);
/** @brief 创建 idle/定时服务、置 g_sched_run、启动 hart0 首任务（不返回） */
void cgrtos_start(void);
/**
 * @brief 从核调度入口：等 g_sched_run 后跑 idle
 * @param hartid 本核号
 */
void cgrtos_start_secondary(int hartid);
/** @brief 初始化每核 idle 任务 TCB 与栈 */
void cgrtos_init_idle_tasks(void);

/**
 * @brief 创建任务并放入就绪队列
 * @param name 任务名
 * @param fn 入口
 * @param arg 参数
 * @param prio 优先级 0..CONFIG_MAX_PRIORITY
 * @param policy 调度策略
 * @return 任务 ID；失败为 (task_id_t)-1
 */
task_id_t cgrtos_task_create(const char *name, task_func_t fn, void *arg,
                             uint8_t prio, sched_policy_t policy);
/** @brief 删除任务 @return pdPASS/pdFAIL */
int cgrtos_task_delete(task_id_t id);
/** @brief 挂起任务 */
int cgrtos_task_suspend(task_id_t id);
/** @brief 恢复挂起任务 */
int cgrtos_task_resume(task_id_t id);
/** @brief 设置优先级 */
int cgrtos_task_set_priority(task_id_t id, uint8_t prio);
/**
 * @brief 设置 CPU 亲和性
 * @param cpu 0xFF=任意核；否则硬绑定
 */
int cgrtos_task_set_affinity(task_id_t id, uint8_t cpu);
/** @brief 设置 EDF 周期并武装释放轮 */
int cgrtos_task_set_period(task_id_t id, tick_t period);
/** @brief 设置 EDF 绝对 deadline */
int cgrtos_task_set_deadline(task_id_t id, tick_t deadline);
/** @brief 由 ID 取 TCB 指针 */
cgrtos_task_t *cgrtos_task_get_handle(task_id_t id);
/** @brief 查询 FreeRTOS 风格状态 */
eTaskState_t cgrtos_task_get_state(task_id_t id);
/** @brief 返回任务当前/归属 run_cpu */
uint8_t cgrtos_task_get_run_cpu(task_id_t id);
/** @brief 栈高水位（未用过的字数估测） */
uint32_t cgrtos_task_get_stack_high_water_mark(task_id_t id);
/** @brief 自愿让出 CPU（ecall） */
void cgrtos_task_yield(void);

/**
 * @brief 相对延时（tick）；ticks==0 仅 yield，不入延迟链
 * @note 仅任务上下文；ISR 中调用无效
 */
void cgrtos_delay(tick_t ticks);
/**
 * @brief 阻塞到绝对系统 tick（g_ticks）；已过期立即返回
 * @note 仅任务上下文
 */
void cgrtos_delay_until_tick(tick_t wake);
/**
 * @brief 相对延时（毫秒）；内部用 mtime 绝对截止 + tick 粗阻塞补齐
 * @note 仅任务上下文
 */
void cgrtos_delay_ms(uint32_t ms);
/**
 * @brief 相对延时（微秒）；短延时忙等，长延时 tick 阻塞 + mtime 补齐
 * @note 仅任务上下文
 */
void cgrtos_delay_us(uint32_t us);
/**
 * @brief 绝对周期延时（FreeRTOS vTaskDelayUntil 语义）
 * @param prev_wake inout 上次唤醒 tick（更新为 next）
 * @param increment 周期；0 则忽略
 * @note 若 next 已过期则不阻塞，仅推进 *prev_wake
 */
void cgrtos_delay_until(tick_t *prev_wake, tick_t increment);

#if CONFIG_USE_TASK_NOTIFICATIONS
/**
 * @brief 发送任务通知并可能唤醒对方
 * @return 写入前的旧 notify_value
 */
uint32_t cgrtos_task_notify(cgrtos_task_t *task, uint32_t value,
                            eNotifyAction_t action);
/**
 * @brief 等待通知
 * @param clear_on_entry 进入时清除的位掩码
 * @param clear_on_exit 退出时清除的位掩码
 * @param value 可选输出当前值
 * @param timeout 超时 tick；portMAX_DELAY 永久等
 * @return 非 0 表示成功拿到通知
 */
uint32_t cgrtos_task_notify_wait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                                 uint32_t *value, tick_t timeout);
/** @brief ISR 安全版任务通知 */
uint32_t cgrtos_task_notify_from_isr(cgrtos_task_t *task, uint32_t value,
                                     eNotifyAction_t action);
#endif

/**
 * @brief 创建计数信号量
 * @param init 初值
 * @param max 上限
 */
cgrtos_sem_t *cgrtos_sem_create(int32_t init, int32_t max);
/** @brief 创建二进制信号量 */
cgrtos_sem_t *cgrtos_sem_create_binary(void);
/** @brief 静态创建信号量（调用方提供对象存储） */
cgrtos_sem_t *cgrtos_sem_create_static(cgrtos_sem_t *sem, int32_t init, int32_t max);
/** @brief P 操作；timeout=0 为尝试 */
int cgrtos_sem_take(cgrtos_sem_t *sem, tick_t timeout);
/** @brief V 操作 */
int cgrtos_sem_give(cgrtos_sem_t *sem);
/** @brief ISR 中 V 操作 */
int cgrtos_sem_give_from_isr(cgrtos_sem_t *sem);
/** @brief 删除信号量并唤醒等待者 */
int cgrtos_sem_delete(cgrtos_sem_t *sem);

/** @brief 创建互斥量 */
cgrtos_mutex_t *cgrtos_mutex_create(void);
/** @brief 静态创建互斥量 */
cgrtos_mutex_t *cgrtos_mutex_create_static(cgrtos_mutex_t *mutex);
/**
 * @brief 加锁（同任务可递归；额外层数上限 CONFIG_MUTEX_MAX_RECURSIVE）
 * @note 首次获得时 recursive=0；每多锁一次 recursive++
 */
int cgrtos_mutex_lock(cgrtos_mutex_t *mutex, tick_t timeout);
/** @brief 解锁（须与 lock 成对；递归时先减深度） */
int cgrtos_mutex_unlock(cgrtos_mutex_t *mutex);
/** @brief 删除互斥量 */
int cgrtos_mutex_delete(cgrtos_mutex_t *mutex);
/**
 * @brief 查询递归额外层数（总持有次数 = 返回值+1；未持有为 0）
 */
uint32_t cgrtos_mutex_get_recursive_count(cgrtos_mutex_t *mutex);
/** @brief 当前持有者 TCB；无人持有返回 NULL */
cgrtos_task_t *cgrtos_mutex_get_holder(cgrtos_mutex_t *mutex);
/**
 * @brief 强制释放某任务持有的全部互斥量（删除安全）
 * @details 恢复 PI、清空 owner/recursive，并 handoff 或失败唤醒等待者
 */
void cgrtos_mutex_force_release_owned(cgrtos_task_t *task);

/**
 * @brief 创建消息队列
 * @param len 元素个数
 * @param item_sz 元素大小
 */
cgrtos_queue_t *cgrtos_queue_create(uint32_t len, uint32_t item_sz);
/** @brief 静态队列（storage 指向 len*item_sz 缓冲） */
cgrtos_queue_t *cgrtos_queue_create_static(cgrtos_queue_t *q, void *storage,
                                           uint32_t len, uint32_t item_sz);
/** @brief 发送消息 */
int cgrtos_queue_send(cgrtos_queue_t *q, const void *data, tick_t timeout);
/** @brief 接收消息 */
int cgrtos_queue_receive(cgrtos_queue_t *q, void *buf, tick_t timeout);
/** @brief ISR 发送 */
int cgrtos_queue_send_from_isr(cgrtos_queue_t *q, const void *data);
/** @brief ISR 接收 */
int cgrtos_queue_receive_from_isr(cgrtos_queue_t *q, void *buf);
/** @brief 查询队列中消息数 */
uint32_t cgrtos_queue_messages_waiting(cgrtos_queue_t *q);
/** @brief 删除队列 */
int cgrtos_queue_delete(cgrtos_queue_t *q);

/**
 * @brief 创建软定时器
 * @param periodic 1=周期，0=单次
 */
cgrtos_timer_t *cgrtos_timer_create(const char *name, timer_cb_t cb, void *arg,
                                    tick_t period, uint8_t periodic);
/** @brief 启动 / 重新武装定时器 */
int cgrtos_timer_start(cgrtos_timer_t *timer);
/** @brief 停止定时器 */
int cgrtos_timer_stop(cgrtos_timer_t *timer);
/** @brief 复位（等价重新 start） */
int cgrtos_timer_reset(cgrtos_timer_t *timer);
/** @brief 修改周期 */
int cgrtos_timer_change_period(cgrtos_timer_t *timer, tick_t period);
/** @brief 删除并回收槽位 */
int cgrtos_timer_delete(cgrtos_timer_t *timer);
/** @brief 初始化时间轮并创建 Tmr Svc 守护任务 */
void cgrtos_timer_init(void);

/** @brief 创建事件组 */
cgrtos_event_group_t *cgrtos_event_group_create(void);
/** @brief 静态创建事件组 */
cgrtos_event_group_t *cgrtos_event_group_create_static(cgrtos_event_group_t *eg);
/** @brief 置位并唤醒匹配等待者 @return 新 flags */
event_flags_t cgrtos_event_group_set(cgrtos_event_group_t *eg, event_flags_t flags);
/** @brief ISR 置位 */
event_flags_t cgrtos_event_group_set_from_isr(cgrtos_event_group_t *eg,
                                              event_flags_t flags);
/** @brief 清位 */
event_flags_t cgrtos_event_group_clear(cgrtos_event_group_t *eg, event_flags_t flags);
/** @brief 等待事件（简化接口） */
event_flags_t cgrtos_event_group_wait(cgrtos_event_group_t *eg, event_flags_t flags,
                                      uint8_t wait_all, tick_t timeout);
/**
 * @brief FreeRTOS 风格事件等待
 * @param clear_on_exit 成功后是否清相关位
 * @param wait_all 1=等全部位，0=任一
 */
event_flags_t cgrtos_event_group_wait_bits(cgrtos_event_group_t *eg, event_flags_t flags,
                                           uint8_t clear_on_exit, uint8_t wait_all,
                                           tick_t timeout);
/** @brief 读取当前 flags */
event_flags_t cgrtos_event_group_get(cgrtos_event_group_t *eg);
/** @brief 删除事件组 */
int cgrtos_event_group_delete(cgrtos_event_group_t *eg);

/**
 * @defgroup cgrtos_stream StreamBuffer / MessageBuffer
 * @brief 字节流与整消息缓冲；可加入 QueueSet
 * @{
 */
/**
 * @brief 创建 StreamBuffer
 * @param size 环形缓冲容量（字节，至少 2）
 * @param trigger 可读字节 ≥ trigger 时唤醒接收者；0 视为 1
 * @return 句柄；池满或堆不足时返回 NULL
 */
cgrtos_stream_buffer_t *cgrtos_stream_buffer_create(uint32_t size, uint32_t trigger);
/**
 * @brief 写入字节流（可部分写入）；空间不足时可阻塞
 * @return 实际写入字节数
 */
size_t cgrtos_stream_buffer_send(cgrtos_stream_buffer_t *sb, const void *data,
                                 size_t len, tick_t timeout);
/** @brief ISR 安全发送；不阻塞，可能只写部分 */
size_t cgrtos_stream_buffer_send_from_isr(cgrtos_stream_buffer_t *sb, const void *data,
                                          size_t len);
/**
 * @brief 读取字节流（可部分读取）；数据不足时可阻塞至 trigger
 * @return 实际读出字节数
 */
size_t cgrtos_stream_buffer_recv(cgrtos_stream_buffer_t *sb, void *buf, size_t len,
                                 tick_t timeout);
/** @brief ISR 安全接收；不阻塞 */
size_t cgrtos_stream_buffer_recv_from_isr(cgrtos_stream_buffer_t *sb, void *buf,
                                          size_t len);
/** @brief 清空缓冲并唤醒等待者 @return pdPASS / pdFAIL */
int cgrtos_stream_buffer_reset(cgrtos_stream_buffer_t *sb);
/** @brief 删除对象并释放堆缓冲 @return pdPASS / pdFAIL */
int cgrtos_stream_buffer_delete(cgrtos_stream_buffer_t *sb);
/** @brief 当前可读字节数 */
uint32_t cgrtos_stream_buffer_bytes_available(cgrtos_stream_buffer_t *sb);
/** @brief 当前可写空闲字节数 */
uint32_t cgrtos_stream_buffer_spaces_available(cgrtos_stream_buffer_t *sb);

/**
 * @brief 创建 MessageBuffer（整消息原子收发；size 需容纳最长消息 + 2 字节头）
 */
cgrtos_message_buffer_t *cgrtos_message_buffer_create(uint32_t size);
/**
 * @brief 发送一整条消息；空间不足则阻塞或返回 0
 * @return 成功为 payload 长度，失败为 0
 */
size_t cgrtos_message_buffer_send(cgrtos_message_buffer_t *mb, const void *data,
                                  size_t len, tick_t timeout);
/**
 * @brief 接收一整条消息；`buf_len` 过小则丢弃该消息并返回 0
 * @return 消息长度；超时/空为 0
 */
size_t cgrtos_message_buffer_recv(cgrtos_message_buffer_t *mb, void *buf, size_t buf_len,
                                  tick_t timeout);
/** @brief 删除 MessageBuffer（同 stream delete） */
int cgrtos_message_buffer_delete(cgrtos_message_buffer_t *mb);
/** @} */

/**
 * @defgroup cgrtos_qset QueueSet
 * @brief 多 IPC 对象就绪选择（类似 select）
 * @{
 */
/**
 * @brief 创建 QueueSet
 * @param length 预留参数；实际容量为 `CGRTOS_QUEUE_SET_LENGTH`
 */
cgrtos_queue_set_t *cgrtos_queue_set_create(uint32_t length);
/** @brief 将队列加入集合；已加入其它 set 则失败 @return pdPASS / pdFAIL */
int cgrtos_queue_set_add_queue(cgrtos_queue_set_t *set, cgrtos_queue_t *q);
/** @brief 将信号量加入集合 */
int cgrtos_queue_set_add_sem(cgrtos_queue_set_t *set, cgrtos_sem_t *sem);
/** @brief 将 Stream/MessageBuffer 加入集合 */
int cgrtos_queue_set_add_stream(cgrtos_queue_set_t *set, cgrtos_stream_buffer_t *sb);
/** @brief 从集合移除成员（按对象指针） */
int cgrtos_queue_set_remove(cgrtos_queue_set_t *set, void *obj);
/**
 * @brief 阻塞直到任一成员就绪
 * @return 就绪成员对象指针；超时返回 NULL
 * @note 返回后仍需对该对象再 take/recv；不自动消费数据
 */
void *cgrtos_queue_set_select(cgrtos_queue_set_t *set, tick_t timeout);
/** @brief 删除 QueueSet（不删除成员对象） */
int cgrtos_queue_set_delete(cgrtos_queue_set_t *set);
/**
 * @brief 成员就绪时内部 poke（由 queue_send / sem_give / stream_send 调用）
 * @note 应用层一般无需直接调用
 */
void cgrtos_queue_set_poke(cgrtos_queue_set_t *set, void *obj);
/** @} */

/**
 * @defgroup cgrtos_fs_api RAM FS API
 * @ingroup cgrtos_fs
 * @{
 */
/** @brief 初始化根 inode 与 FS 互斥；`cgrtos_init` 已调用，可重复安全调用 */
void cgrtos_fs_init(void);
/**
 * @brief 打开/创建文件
 * @param path 绝对路径（须以 `/` 开头）
 * @param flags `CGRTOS_O_*` 组合
 * @return ≥0 文件描述符；失败 -1
 */
int cgrtos_fs_open(const char *path, int flags);
/** @brief 关闭 fd @return 0 成功，-1 失败 */
int cgrtos_fs_close(int fd);
/** @brief 读文件 @return 读出字节数，失败 -1 */
int cgrtos_fs_read(int fd, void *buf, size_t len);
/** @brief 写文件（可增长至 `CGRTOS_FS_MAX_FILE_BYTES`） @return 写出字节数，失败 -1 */
int cgrtos_fs_write(int fd, const void *buf, size_t len);
/**
 * @brief 定位
 * @param whence 0=SET，1=CUR，2=END
 * @return 新偏移，失败 -1
 */
long cgrtos_fs_lseek(int fd, long off, int whence);
/** @brief 删除普通文件 @return 0 成功，-1 失败 */
int cgrtos_fs_unlink(const char *path);
/** @brief 创建目录（父目录须存在） */
int cgrtos_fs_mkdir(const char *path);
/** @brief 删除空目录 */
int cgrtos_fs_rmdir(const char *path);
/** @brief 查询 path 类型与大小 */
int cgrtos_fs_stat(const char *path, cgrtos_stat_t *st);
/** @brief 打开目录迭代 @return 句柄或 NULL */
cgrtos_dir_t *cgrtos_fs_opendir(const char *path);
/**
 * @brief 读取下一目录项
 * @return 1 有项，0 结束，-1 失败
 */
int cgrtos_fs_readdir(cgrtos_dir_t *dir, cgrtos_dirent_t *out);
/** @brief 关闭目录句柄 */
int cgrtos_fs_closedir(cgrtos_dir_t *dir);
/** @} */

/** @brief TLSF 分配 @return 用户指针或 NULL */
void *cgrtos_malloc(unsigned long size);
/** @brief 分配并清零 */
void *cgrtos_calloc(unsigned long count, unsigned long size);
/** @brief 释放；NULL 安全 */
void cgrtos_free(void *ptr);
/** @brief 当前空闲堆字节数 */
unsigned long cgrtos_get_free_heap(void);
/** @brief 历史最小空闲堆 */
unsigned long cgrtos_get_min_free_heap(void);

#if CONFIG_USE_HOOKS
/** @brief 注册 idle 钩子（可为 NULL） */
void cgrtos_set_idle_hook(cgrtos_hook_fn_t hook);
/** @brief 注册 tick 钩子（hart0 tick 路径） */
void cgrtos_set_tick_hook(cgrtos_hook_fn_t hook);
/** @brief 注册 malloc 失败钩子 */
void cgrtos_set_malloc_failed_hook(cgrtos_malloc_failed_hook_t hook);
#endif
/** @brief 注册断言失败钩子（halt 前调用） */
void cgrtos_set_assert_hook(cgrtos_assert_hook_t hook);
/** @brief 注册栈溢出钩子 */
void cgrtos_set_stack_overflow_hook(cgrtos_stack_overflow_hook_t hook);

/** @brief 打印运行时统计到 UART */
void cgrtos_stats_dump(void);
/**
 * @brief 填充运行时统计快照
 * @param out 输出结构；NULL 则忽略
 */
void cgrtos_stats_get(cgrtos_runtime_stats_t *out);
/** @brief 任务累计运行 tick（exec 字段） */
tick_t cgrtos_task_get_runtime(task_id_t id);
/**
 * @brief 检查任务栈金丝雀是否完好
 * @return pdPASS 正常；pdFAIL 溢出或无效任务
 */
int cgrtos_task_check_stack(task_id_t id);
/**
 * @brief 处理栈溢出（计数、钩子、默认 assert halt）
 * @note 调度器切换 / tick 抽检路径调用
 */
void cgrtos_task_handle_stack_overflow(cgrtos_task_t *task);
/**
 * @brief 切换离开已删除任务时回收 TCB 槽（清 id）
 * @note 仅当该 TCB 不再是任意核的 g_current 时真正清零
 */
void cgrtos_task_reclaim_deleted(cgrtos_task_t *task);
/** @brief 置 yield 标志并 ecall */
void cgrtos_sched_yield(void);
/** @brief ISR 中请求重新调度 */
void cgrtos_sched_yield_from_isr(void);
/**
 * @brief Trap 出口选下一个任务并返回其栈指针
 * @param sp 当前 trap 帧栈
 * @return 下一个任务的 sp（可与入参相同）
 */
uint64_t *cgrtos_sched_switch_from_trap(uint64_t *sp);
/** @brief 挂起调度器（嵌套计数） */
void cgrtos_sched_suspend(void);
/** @brief 恢复调度器 */
void cgrtos_sched_resume(void);
/** @brief 加权 Push 负载均衡（通常 hart0 tick 周期调用） */
void cgrtos_sched_load_balance(void);
/** @brief Idle 工作窃取（受 CONFIG_SMP_IDLE_STEAL 控制） */
void cgrtos_sched_idle_steal(void);
/** @brief 统计某核 READY 任务数 */
uint32_t cgrtos_sched_ready_count(uint8_t cpu);
/** @brief 计算某核加权负载 */
uint32_t cgrtos_sched_core_load(uint8_t cpu);
/** @brief 返回当前最轻载核号 */
uint8_t cgrtos_sched_least_loaded_core(void);
/** @brief 重新发布 boot 魔数并等待 hart1 stage>=4 */
void cgrtos_smp_kick_secondaries(void);
/** @brief 将 EDF 任务下次释放挂入 wheel */
void cgrtos_sched_edf_arm(cgrtos_task_t *task);
/** @brief 向指定核写 CLINT MSIP */
void cgrtos_smp_send_ipi(uint8_t core);
/** @brief 节拍处理：延时/EDF/定时器/均衡/时间片 */
/** @brief 系统 tick 中断处理（hart0 全局 + 本核切片） */
void cgrtos_tick_handler(void);
/** @brief 本核时间片记账（供次核 IPI 远程 tick 调用） */
void cgrtos_tick_local(void);
/** @brief ISR 嵌套计数 +1 */
void cgrtos_isr_enter(void);
/** @brief ISR 嵌套计数 -1 */
void cgrtos_isr_exit(void);

/** @brief 自旋加锁 */
void cgrtos_spin_lock(spinlock_t *lock);
/** @brief 自旋解锁 */
void cgrtos_spin_unlock(spinlock_t *lock);
/** @brief 保存 mstatus 并清 MIE @return 旧 mstatus */
uint64_t cgrtos_irq_save(void);
/** @brief 恢复 mstatus */
void cgrtos_irq_restore(uint64_t flags);
/** @brief 进入可嵌套临界区（关 IRQ + g_klock） */
void cgrtos_enter_critical(void);
/** @brief 退出临界区 */
void cgrtos_exit_critical(void);
/** @brief 本核是否在临界区内 */
int cgrtos_in_critical(void);
/** @brief 是否在 ISR 上下文 */
int cgrtos_in_isr(void);

/** @brief 体系结构早期初始化（开 MIE 位等） */
void cgrtos_arch_init(void);
/** @brief 初始化 UART */
void cgrtos_uart_init(void);
/** @brief 输出一字符 */
void cgrtos_uart_putc(char c);
/** @brief 阻塞读一字符（调度运行后会 yield，避免饿死同核任务） */
char cgrtos_uart_getc(void);
/**
 * @brief 非阻塞读一字符
 * @return 0..255 为数据；-1 表示 RX 空
 */
int cgrtos_uart_pollc(void);
/** @brief 输出字符串 */
void cgrtos_uart_puts(const char *s);
/** @brief 简易格式化打印（支持 s/d/u/x/p/c 及长整型变体） */
void cgrtos_printf(const char *fmt, ...);
/** @brief 初始化本核 mtimecmp 与 MTIE */
void cgrtos_clint_init(tick_t rate);
/** @brief 读 SysTimer mtime */
uint64_t cgrtos_mtime_read(void);
/** @brief 初始化 PLIC */
void cgrtos_plic_init(void);
/** @brief PLIC claim */
uint32_t cgrtos_plic_claim(void);
/** @brief PLIC complete */
void cgrtos_plic_complete(uint32_t irq);
/** @brief 原子读取 g_ticks */
tick_t cgrtos_get_ticks(void);
/** @brief 有界 snprintf 风格格式化 */
int cgrtos_snprintf(char *buf, unsigned long n, const char *fmt, ...);

/** @brief 断言失败：打印后死循环 */
void cgrtos_assert_failed(const char *file, int line);

/* --- Scheduler internals（IPC / task 使用） --- */

/** @brief 任务应入队的目标核（亲和性解析） */
uint8_t cgrtos_sched_target_core(cgrtos_task_t *task);
/** @brief 按策略插入就绪结构 */
void cgrtos_sched_add_ready(cgrtos_task_t *task);
/** @brief 从就绪结构移除 */
void cgrtos_sched_remove_ready(cgrtos_task_t *task);
/**
 * @brief 阻塞当前任务
 * @param timeout 0=立即；portMAX_DELAY=无限；否则相对 tick
 */
void cgrtos_sched_block(cgrtos_task_t *task, block_reason_t reason,
                        void *obj, tick_t timeout);
/**
 * @brief 阻塞到绝对 wake_tick（g_ticks）
 * @note 调用前应保证 wake_tick > g_ticks
 */
void cgrtos_sched_block_until(cgrtos_task_t *task, block_reason_t reason,
                              void *obj, tick_t wake_tick);
/** @brief 解除阻塞并可选跨核 IPI @return pdPASS/pdFAIL */
int cgrtos_sched_unblock(cgrtos_task_t *task);
/** @brief 等待队列 FIFO 入队 */
void cgrtos_wait_list_add(cgrtos_task_t *volatile *head, cgrtos_task_t *task);
/** @brief 等待队列按优先级插入 */
void cgrtos_wait_list_add_priority(cgrtos_task_t *volatile *head, cgrtos_task_t *task);
/** @brief 弹出最高优先级等待者 */
cgrtos_task_t *cgrtos_wait_list_pop_highest(cgrtos_task_t *volatile *head);
/** @brief 从等待队列摘除 */
void cgrtos_wait_list_remove(cgrtos_task_t *volatile *head, cgrtos_task_t *task);
/** @brief 删除/超时前清理任务在各 IPC 等待链上的节点 */
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
/** @def xSemaphoreGiveFromISR ISR 中释放信号量 */
#define xSemaphoreGiveFromISR(s, hp) \
    ((void)(hp), cgrtos_sem_give_from_isr(s))
/** @def vSemaphoreDelete 删除信号量 */
#define vSemaphoreDelete(s)         cgrtos_sem_delete(s)

/** @def xQueueCreate 创建消息队列 */
#define xQueueCreate(len, sz)       cgrtos_queue_create(len, sz)
/** @def xQueueSend 发送到队列 */
#define xQueueSend(q, d, t)         cgrtos_queue_send(q, d, t)
/** @def xQueueReceive 从队列接收 */
#define xQueueReceive(q, b, t)      cgrtos_queue_receive(q, b, t)
/** @def xQueueSendFromISR ISR 发送 */
#define xQueueSendFromISR(q, d, hp) \
    ((void)(hp), cgrtos_queue_send_from_isr(q, d))
/** @def xQueueReceiveFromISR ISR 接收 */
#define xQueueReceiveFromISR(q, b, hp) \
    ((void)(hp), cgrtos_queue_receive_from_isr(q, b))
/** @def uxQueueMessagesWaiting 队列当前消息数 */
#define uxQueueMessagesWaiting(q)   cgrtos_queue_messages_waiting(q)
/** @def vQueueDelete 删除队列 */
#define vQueueDelete(q)             cgrtos_queue_delete(q)

/** @def xEventGroupCreate 创建事件组 */
#define xEventGroupCreate()         cgrtos_event_group_create()
/** @def xEventGroupSetBits 置事件位 */
#define xEventGroupSetBits(eg, f)   cgrtos_event_group_set(eg, f)
/** @def xEventGroupSetBitsFromISR ISR 置事件位 */
#define xEventGroupSetBitsFromISR(eg, f, hp) \
    ((void)(hp), cgrtos_event_group_set_from_isr(eg, f))
/** @def xEventGroupClearBits 清事件位 */
#define xEventGroupClearBits(eg, f) cgrtos_event_group_clear(eg, f)
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

/** @def portYIELD_FROM_ISR ISR 末请求调度（hp 参数忽略） */
#define portYIELD_FROM_ISR(hp) \
    do { (void)(hp); cgrtos_sched_yield_from_isr(); } while (0)

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
