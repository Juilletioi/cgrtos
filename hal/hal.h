/**
 * @file hal.h
 * @brief CG-RTOS 统一外设驱动框架与用户 HAL API
 *
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @ingroup hal
 *
 * @par 分层模型（箭头 = 调用方向；禁止倒置）
 * @code
 *   App / Kernel / Tests
 *        |  推荐： hal_*        兼容：cgrtos_uart_* / cgrtos_plic_* ...
 *        v
 *   HAL 用户 API（本头文件 + hal_core.c / hal_compat.c / hal_printf.c）
 *        |  错误码、锁、注册表；boot 时调用 drv_*_device() 并 register
 *        v
 *   Board drivers (arch/riscv)-- 只实现 ops + MMIO（hal_board.h）；不调用 hal_*
 *        |
 *        v
 *   硬件
 *
 *   Trap / 极底层 ISR 入口 ──直调──> 驱动内部 *_hw_*（不经 hal_irqc_* 等）
 * @endcode
 *
 * @par 设计目标
 * - 易用：按子系统命名（console / timer / irqc / ipi），返回统一 hal_status_t
 * - 可移植：换 SoC 只改 hal_board.h + 对应 arch 驱动；本头保持稳定
 * - 鲁棒：参数校验、未就绪返回 HAL_ERR_NODEV、注册表可冻结防误改
 * - 多核安全：见各 API 注释；控制台整行输出可原子化
 *
 * @par 并发模型（务必阅读）
 * | 类别 | Boot（hal_board_init 前） | 运行期多任务/多核 | ISR |
 * |------|---------------------------|-------------------|-----|
 * | 设备注册 | 仅 hart0 | 禁止（冻结后失败） | 禁止 |
 * | console 输出 | 可用 | 有锁；puts/write/printf 整段原子 | 可用但勿长时间占锁 |
 * | mtime 读 | 可用 | 无锁（RV64 原子读） | 安全 |
 * | irqc claim/complete/threshold | - | 每 hart 独立 | ISR 路径，无全局锁 |
 * | irqc enable/priority | 建议 boot 配置 | 配置锁保护 RMW | 勿在 ISR 改表 |
 * | ipi send/clear | - | 字写；非法 hart 返回错误 | clear 常在本核 ISR |
 *
 * @warning 控制台锁不是 g_klock，避免 printf 嵌在 enter_critical 内死锁。
 * @warning 持有控制台锁时禁止 yield / 长时间阻塞 / 再取内核大锁。
 *
 * @note 历史 cgrtos_* 外设 API 为薄封装，安全契约与对应 hal_* 一致。
 * @note 板级 MMIO 见单独头 hal/hal_board.h（驱动与移植用；应用通常不必直接包含）。
 */
#ifndef CGRTOS_HAL_H
#define CGRTOS_HAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Status codes                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief HAL 统一状态码（0 成功，负值失败）
 * @note 与内核 pdPASS(1)/pdFAIL(0) 不同；兼容层会做转换。
 */
typedef int hal_status_t;

/**
 * @brief 操作成功
 * @warning 无运行时副作用（仅整型常量）
 */
#define HAL_OK           0
/**
 * @brief 参数非法
 * @warning 无运行时副作用（仅整型常量）
 */
#define HAL_ERR_PARAM  (-1)
/**
 * @brief 设备未注册或未就绪
 * @warning 无运行时副作用（仅整型常量）
 */
#define HAL_ERR_NODEV  (-2)
/**
 * @brief 硬件 I/O 失败（预留）
 * @warning 无运行时副作用（仅整型常量）
 */
#define HAL_ERR_IO     (-3)
/**
 * @brief 资源忙（预留）
 * @warning 无运行时副作用（仅整型常量）
 */
#define HAL_ERR_BUSY   (-4)
/**
 * @brief 状态不允许（如冻结后再注册）
 * @warning 无运行时副作用（仅整型常量）
 */
#define HAL_ERR_STATE  (-5)

/* -------------------------------------------------------------------------- */
/* Device framework                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief 设备类别（每板通常每类一个实例；find 返回首个匹配）
 */
typedef enum {
    HAL_DEV_CPU     = 0, /**< CPU / mie 早期配置 */
    HAL_DEV_CONSOLE = 1, /**< 调试控制台 UART */
    HAL_DEV_TIMER   = 2, /**< 系统节拍定时器（mtime/mtimecmp） */
    HAL_DEV_IRQC    = 3, /**< 外部中断控制器（PLIC） */
    HAL_DEV_IPI     = 4, /**< 核间软件中断（MSIP） */
    HAL_DEV_CLASS_MAX    /**< 类别上界（非法值哨兵，非真实设备类） */
} hal_dev_class_t;

/**
 * @brief 设备已完成硬件 init 标志位
 * @warning 无运行时副作用（仅位标志常量）
 */
#define HAL_DEV_F_READY   0x01U
/**
 * @brief 设备已进入注册表标志位
 * @warning 无运行时副作用（仅位标志常量）
 */
#define HAL_DEV_F_REG     0x02U

struct hal_device;

/**
 * @brief 控制台驱动 ops
 * @note putc/pollc 由 HAL 在锁内调用；驱动实现不必再加锁。
 */
typedef struct {
    /**
     * @brief 硬件初始化控制台
     * @details 配置 UART 波特率/FIFO 等；由 hal_console_init / hal_board_init 调用。
     * @param[in,out] dev 控制台设备描述符
     * @return HAL_OK 或负错误码
     * @retval HAL_OK 初始化成功
     * @retval HAL_ERR_* 硬件或参数失败
     * @note 驱动实现；勿再调用 hal_*
     * @warning 通常仅 boot 调用一次
     * @attention ❌ ISR；❌ block/switch
     */
    hal_status_t (*init)(struct hal_device *dev);
    /**
     * @brief 阻塞发送一字符
     * @details 写 TX FIFO；建议对 '\\n' 追加 '\\r'。由 HAL 在控制台锁内调用。
     * @param[in,out] dev 控制台设备描述符
     * @param[in] c 待发送字符
     * @return 无
     * @retval 无
     * @note 驱动不必再加锁
     * @warning 可能轮询 TXFULL，短暂自旋
     * @attention ✅ ISR（调用方已持锁）；❌ block/switch（勿再取锁）
     */
    void (*putc)(struct hal_device *dev, char c);
    /**
     * @brief 非阻塞接收一字符
     * @details 读 RX FIFO；空则返回 -1。由 HAL 在控制台锁内调用。
     * @param[in,out] dev 控制台设备描述符
     * @return 0..255 数据；-1 表示 RX 空
     * @retval 0..255 收到字节
     * @retval -1 RX 空（勿与 HAL_ERR_* 混淆）
     * @note 驱动不必再加锁
     * @warning 无
     * @attention ✅ ISR（调用方已持锁）；❌ 不阻塞
     */
    int (*pollc)(struct hal_device *dev);
} hal_console_ops_t;

/**
 * @brief 系统定时器 ops
 * @note mtime_read 必须可在任意上下文无锁调用（只读 MMIO）。
 */
typedef struct {
    /**
     * @brief 为本 hart 配置 tick 并打开本地定时器中断使能
     * @details 写 mtimecmp / 等价寄存器并开启 MTIE；板级可用 CONFIG_TICK_RATE_HZ。
     * @param[in,out] dev 定时器设备描述符
     * @param[in] tick_hz 期望频率；板级可忽略并用 CONFIG_TICK_RATE_HZ
     * @return HAL_OK 或负错误码
     * @retval HAL_OK 初始化成功
     * @retval HAL_ERR_* 失败
     * @note 由 hal_timer_init / hal_board_init 调用
     * @warning 通常仅 boot 每核调用
     * @attention ❌ ISR；❌ block/switch
     */
    hal_status_t (*init)(struct hal_device *dev, uint32_t tick_hz);
    /**
     * @brief 读自由运行时间计数器
     * @details 无锁只读 mtime（或 CNTV 等）；任意上下文安全。
     * @param[in] dev 定时器设备描述符
     * @return 64 位时间计数
     * @retval uint64_t 当前计数
     * @note 无全局锁
     * @warning 无
     * @attention ✅ ISR；❌ 不阻塞
     */
    uint64_t (*mtime_read)(struct hal_device *dev);
} hal_timer_ops_t;

/**
 * @brief 外部中断控制器 ops
 * @note claim/complete/threshold 面向当前 hart；enable/priority 含跨源共享寄存器。
 */
typedef struct {
    /**
     * @brief 初始化本 hart IRQC 上下文
     * @details 清 threshold、按板级默认配置等；由 hal_irqc_init 调用。
     * @param[in,out] dev IRQC 设备描述符
     * @return HAL_OK 或负错误码
     * @retval HAL_OK 成功
     * @retval HAL_ERR_* 失败
     * @note 驱动实现；勿再调用 hal_*
     * @warning 通常仅 boot 调用
     * @attention ❌ ISR；❌ block/switch
     */
    hal_status_t (*init)(struct hal_device *dev);
    /**
     * @brief claim 最高优先级 pending 源
     * @details 读本 hart claim/complete 寄存器；0 表示无 pending。
     * @param[in,out] dev IRQC 设备描述符
     * @return 源 ID；0=无
     * @retval >0 已 claim 源号
     * @retval 0 无 pending
     * @note trap 入口应直调驱动 hw 例程，勿为图省事再绕回 hal_irqc_claim
     * @warning complete 前勿重复 claim 同一源
     * @attention ✅ ISR；❌ 不阻塞
     */
    uint32_t (*claim)(struct hal_device *dev);
    /**
     * @brief 完成中断处理并写 complete
     * @details 向本 hart complete 寄存器写回先前 claim 的源号。
     * @param[in,out] dev IRQC 设备描述符
     * @param[in] irq 先前 claim 的源号
     * @return 无
     * @retval 无
     * @note 须与 claim 配对
     * @warning irq 须为最近一次 claim 的源
     * @attention ✅ ISR；❌ 不阻塞
     */
    void (*complete)(struct hal_device *dev, uint32_t irq);
    /**
     * @brief 设置本 hart 优先级阈值
     * @details 写 threshold 寄存器；0 通常允许全部 priority>0。
     * @param[in,out] dev IRQC 设备描述符
     * @param[in] thr 新阈值
     * @return 无
     * @retval 无
     * @note 每 hart 独立，无全局锁
     * @warning 无
     * @attention ✅ ISR；❌ 不阻塞
     */
    void (*set_threshold)(struct hal_device *dev, uint32_t thr);
    /**
     * @brief 读本 hart 优先级阈值
     * @details 读 threshold 寄存器。
     * @param[in] dev IRQC 设备描述符
     * @return 当前阈值
     * @retval uint32_t 阈值
     * @note 只读
     * @warning 无
     * @attention ✅ ISR；❌ 不阻塞
     */
    uint32_t (*get_threshold)(struct hal_device *dev);
    /**
     * @brief 设置中断源优先级
     * @details 写共享 priority 表；HAL 侧持配置锁保护 RMW。
     * @param[in,out] dev IRQC 设备描述符
     * @param[in] irq 源号
     * @param[in] prio 优先级（通常 0..CONFIG_IRQ_PRIORITY_MAX）
     * @return HAL_OK / HAL_ERR_PARAM 等
     * @retval HAL_OK 成功
     * @retval HAL_ERR_PARAM irq/prio 非法
     * @note 建议 boot/任务上下文配置
     * @warning 勿在 ISR 改表
     * @attention ❌ ISR；❌ 不阻塞
     */
    hal_status_t (*set_priority)(struct hal_device *dev, uint32_t irq, uint32_t prio);
    /**
     * @brief 读中断源优先级
     * @details 读共享 priority 表。
     * @param[in] dev IRQC 设备描述符
     * @param[in] irq 源号
     * @return 优先级；非法可为 0
     * @retval uint32_t 当前优先级
     * @note 只读
     * @warning 非法 irq 行为取决于驱动
     * @attention ✅ ISR；❌ 不阻塞
     */
    uint32_t (*get_priority)(struct hal_device *dev, uint32_t irq);
    /**
     * @brief 对本 hart 使能中断源
     * @details 置 enable 位图对应 bit；HAL 侧持配置锁。
     * @param[in,out] dev IRQC 设备描述符
     * @param[in] irq 源号
     * @return HAL_OK / HAL_ERR_PARAM 等
     * @retval HAL_OK 成功
     * @retval HAL_ERR_PARAM irq 非法
     * @note per-hart enable
     * @warning 勿在 ISR 改表
     * @attention ❌ ISR；❌ 不阻塞
     */
    hal_status_t (*enable)(struct hal_device *dev, uint32_t irq);
    /**
     * @brief 对本 hart 禁用中断源
     * @details 清 enable 位图对应 bit；HAL 侧持配置锁。
     * @param[in,out] dev IRQC 设备描述符
     * @param[in] irq 源号
     * @return HAL_OK / HAL_ERR_PARAM 等
     * @retval HAL_OK 成功
     * @retval HAL_ERR_PARAM irq 非法
     * @note per-hart disable
     * @warning 勿在 ISR 改表
     * @attention ❌ ISR；❌ 不阻塞
     */
    hal_status_t (*disable)(struct hal_device *dev, uint32_t irq);
} hal_irqc_ops_t;

/**
 * @brief IPI（核间软件中断）ops
 */
typedef struct {
    /**
     * @brief 初始化 IPI 硬件
     * @details 可选；无特殊硬件时可空实现返回 HAL_OK。
     * @param[in,out] dev IPI 设备描述符
     * @return HAL_OK 或负错误码
     * @retval HAL_OK 成功
     * @retval HAL_ERR_* 失败
     * @note 由 hal_board_init 可选调用
     * @warning 通常仅 boot 调用
     * @attention ❌ ISR；❌ block/switch
     */
    hal_status_t (*init)(struct hal_device *dev);
    /**
     * @brief 向目标 hart 写 MSIP=1
     * @details 触发目标核软件中断；非法 hart 返回 HAL_ERR_PARAM。
     * @param[in,out] dev IPI 设备描述符
     * @param[in] hart 目标 hart 编号
     * @return HAL_OK / HAL_ERR_PARAM
     * @retval HAL_OK 成功
     * @retval HAL_ERR_PARAM hart 越界
     * @note 无全局 HAL 锁
     * @warning 目标 hart 须已上线并可收 IPI
     * @attention ✅ ISR；❌ 不阻塞
     */
    hal_status_t (*send)(struct hal_device *dev, uint8_t hart);
    /**
     * @brief 清目标 hart MSIP（通常清本核）
     * @details 写 MSIP=0；常在本核 IPI ISR 末尾调用。
     * @param[in,out] dev IPI 设备描述符
     * @param[in] hart 目标 hart 编号
     * @return 无
     * @retval 无
     * @note 无全局 HAL 锁
     * @warning hart 越界时底层行为未定义
     * @attention ✅ ISR；❌ 不阻塞
     */
    void (*clear)(struct hal_device *dev, uint8_t hart);
} hal_ipi_ops_t;

/**
 * @brief CPU 早期初始化 ops
 */
typedef struct {
    /**
     * @brief 早期打开架构相关中断使能（如 MSIE|MTIE|MEIE）
     * @details 写 mie/等效寄存器；须在 trap 向量就绪后调用。
     * @param[in,out] dev CPU 设备描述符
     * @return HAL_OK 或负错误码
     * @retval HAL_OK 成功
     * @retval HAL_ERR_* 失败
     * @note 由 hal_cpu_init / hal_board_init 最先调用之一
     * @warning 须在 trap 向量就绪后调用
     * @attention ❌ ISR；❌ block/switch
     */
    hal_status_t (*init)(struct hal_device *dev);
} hal_cpu_ops_t;

/**
 * @brief 统一设备描述符（静态分配，注册后生命周期 = 系统寿命）
 */
typedef struct hal_device {
    const char *name;           /**< 唯一短名，如 "uart0" */
    hal_dev_class_t class;      /**< 设备类别（hal_dev_class_t） */
    uintptr_t mmio_base;        /**< 主 MMIO 基址（诊断 / 移植核对） */
    const void *ops;            /**< 类别专属 ops 指针，不可为 NULL */
    void *priv;                 /**< 驱动私有数据；可为 NULL */
    uint8_t flags;              /**< HAL_DEV_F_* 标志组合 */
} hal_device_t;

#ifndef HAL_DEVICE_MAX
/**
 * @brief 注册表最大设备槽位数
 * @warning 无运行时副作用（编译期容量常量）；改大需同步静态表
 */
#define HAL_DEVICE_MAX  8
#endif

#ifndef HAL_CONSOLE_PRINTF_BUF
/**
 * @brief 控制台 printf 内部缓冲字节数（预留/参考）
 * @warning 无运行时副作用（编译期常量）
 */
#define HAL_CONSOLE_PRINTF_BUF  256
#endif

/**
 * @brief 注册设备（仅 boot / hal_board_init 完成前）
 * @details
 * 1. 校验 dev / name / ops / class 范围。
 * 2. 关本核中断并取注册自旋锁。
 * 3. 若已冻结返回 HAL_ERR_STATE；同名已存在视为幂等 HAL_OK。
 * 4. 表满返回 HAL_ERR_STATE；否则写入注册表并缓存类别指针。
 * @param[in] dev 静态寿命的设备描述符指针；name/ops/class 必填
 * @return HAL_OK；参数错 HAL_ERR_PARAM；表满或冻结 HAL_ERR_STATE
 * @retval HAL_OK 注册成功或同名幂等
 * @retval HAL_ERR_PARAM 参数非法
 * @retval HAL_ERR_STATE 表满或注册表已冻结
 * @note 驱动文件不得调用本函数；由 hal_board_init 统一调用
 * @warning 冻结后调用恒返回 HAL_ERR_STATE；仍应只在 hart0 启动路径调用
 * @attention ❌ ISR；❌ block/switch
 */
hal_status_t hal_device_register(hal_device_t *dev);

/**
 * @brief 按类别查找（返回第一个）
 * @details 先查类别缓存；未命中则线性扫描注册表按 class 匹配。
 * @param[in] cls 设备类别枚举
 * @return 设备指针；未找到 NULL
 * @retval 非 NULL 首个匹配该类别的设备
 * @retval NULL 无此类别或 cls 越界
 * @note 注册表冻结后只读，可无锁并发读
 * @warning 返回指针为静态描述符，勿 free
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *hal_device_find(hal_dev_class_t cls);

/**
 * @brief 按名称查找已注册设备
 * @details 线性扫描注册表，按 name 字符串完全匹配。
 * @param[in] name 设备名；NULL 时返回 NULL
 * @return 设备指针；未找到 NULL
 * @retval 非 NULL 同名设备
 * @retval NULL 未找到或 name 为 NULL
 * @note 冻结前后均可只读调用
 * @warning 返回指针为静态描述符，勿 free
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *hal_device_find_by_name(const char *name);

/**
 * @brief 返回当前已注册设备数量
 * @details 只读返回注册表计数。
 * @return 已注册条目数（0..HAL_DEVICE_MAX）
 * @retval >=0 当前表内设备数
 * @note 冻结前后均可调用
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int hal_device_count(void);

/**
 * @brief 按索引取设备（0 .. count-1）
 * @details 边界检查后返回注册表槽位指针；越界返回 NULL。
 * @param[in] index 从 0 起的表下标
 * @return 设备指针；越界 NULL
 * @retval 非 NULL 对应槽位设备
 * @retval NULL index 非法
 * @note 用于枚举/调试
 * @warning 返回指针为静态描述符
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *hal_device_get(int index);

/**
 * @brief 查询注册表是否已冻结
 * @details 只读检查冻结标志；hal_board_init 末尾置位。
 * @return 非 0 表示已冻结；0 表示仍可注册
 * @retval 1 已冻结
 * @retval 0 未冻结
 * @note 冻结后禁止再 register
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int hal_registry_frozen(void);

/**
 * @brief 板级一键初始化：注册、按序 init、冻结注册表
 * @details 步骤（幂等；已初始化则直接返回 HAL_OK）：
 * 1. 调用各 `drv_*_device()` 取得静态设备描述符。
 * 2. `hal_device_register` 写入注册表（驱动本身不注册）。
 * 3. 按序 init：CPU → Console → IRQC → Timer → IPI。
 * 4. 置冻结标志，禁止运行期再注册。
 * @return HAL_OK 或首个失败码（尽量继续注册/初始化其余设备）
 * @retval HAL_OK 全部成功或已初始化
 * @retval HAL_ERR_* 任一 register/init 失败（保留首个错误）
 * @note 此函数是 HAL 调用驱动的入口；trap 路径不得反向调用本函数
 * @warning 仅 hart0 启动路径调用一次；重复调用安全但不再注册新设备
 * @attention ❌ ISR；❌ block/switch
 */
hal_status_t hal_board_init(void);


/* -------------------------------------------------------------------------- */
/* Console                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化控制台硬件
 * @details 查找 CONSOLE 设备并调用 ops->init；成功则置 HAL_DEV_F_READY。
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 初始化成功
 * @retval HAL_ERR_NODEV 无设备或无 init 回调
 * @note boot 阶段由 hal_board_init 调用
 * @warning 重复 init 行为取决于驱动 ops
 * @attention ❌ ISR；❌ block/switch
 */
hal_status_t hal_console_init(void);

/**
 * @brief 输出一字符（自动加锁）
 * @details 取得控制台锁后调用 putc_unlocked，再释放锁。
 * @param[in] c 待输出字符
 * @return 无
 * @retval 无
 * @note SMP 下整字符输出原子；整行请用 puts/write
 * @warning 持锁期间禁止 yield
 * @attention ✅ ISR；❌ block/switch（持锁勿长时间占用/yield）
 */
void hal_console_putc(char c);

/**
 * @brief 已持锁时输出一字符
 * @details 直接调用 CONSOLE ops->putc；无设备则静默返回。
 * @param[in] c 待输出字符
 * @return 无
 * @retval 无
 * @note 供 puts/write/printf 持锁路径使用
 * @warning 未持锁调用会导致数据竞争
 * @attention ✅ ISR（须已持锁）；❌ block/switch（勿嵌套取锁）
 */
void hal_console_putc_unlocked(char c);

/**
 * @brief 非阻塞读一字符
 * @details 持锁后调用 ops->pollc；空或未就绪返回 -1。
 * @return 0..255；RX 空或未就绪返回 -1
 * @retval >=0 收到字节
 * @retval -1 无数据或设备不可用
 * @note 单次 poll，不 yield
 * @warning 持锁期间禁止 yield
 * @attention ✅ ISR；❌ 不阻塞
 */
int hal_console_pollc(void);

/**
 * @brief 阻塞读一字符；调度运行后空闲时 yield
 * @details 循环 pollc；无数据且调度已启动时 cgrtos_task_yield，直至收到字节。
 * @return 收到的字符（低 8 位）
 * @retval char 成功读到的字符
 * @note 等待期间不持控制台锁
 * @warning 调度未启动时忙等
 * @attention ❌ ISR；✅ block/switch
 */
char hal_console_getc(void);

/**
 * @brief 输出 NUL 字符串（整串持锁，行级原子）
 * @details NULL 忽略；否则持锁逐字节 putc_unlocked 后解锁。
 * @param[in] s NUL 结尾字符串；NULL 安全忽略
 * @return 无
 * @retval 无
 * @note SMP 下整串不会半行交错
 * @warning 持锁期间禁止 yield
 * @attention ✅ ISR；❌ block/switch（持锁勿长时间占用/yield）
 */
void hal_console_puts(const char *s);

/**
 * @brief 输出原始缓冲区（整段持锁）
 * @details len==0 返回 0；buf 为 NULL 且 len>0 返回 HAL_ERR_PARAM；否则持锁写出。
 * @param[in] buf 数据源
 * @param[in] len 字节数
 * @return 写出字节数；失败负状态码
 * @retval (int)len 全部写出
 * @retval 0 len 为 0
 * @retval HAL_ERR_PARAM buf 为 NULL 且 len>0
 * @note 与 puts 相同锁策略
 * @warning 持锁期间禁止 yield
 * @attention ✅ ISR；❌ block/switch（持锁勿长时间占用/yield）
 */
int hal_console_write(const void *buf, size_t len);

/**
 * @brief 获取控制台输出锁（不可重入）
 * @details 关本核中断并自旋获取控制台锁（非 g_klock）。
 * @return 无
 * @retval 无
 * @note 与 hal_console_unlock 成对调用
 * @warning 持锁期间禁止 yield / 再取 g_klock / 长时间阻塞；不可重入
 * @attention ❌ ISR 勿与任务争锁；❌ block/switch
 */
void hal_console_lock(void);

/**
 * @brief 释放控制台输出锁
 * @details 释放自旋锁并恢复进入 lock 时的中断状态。
 * @return 无
 * @retval 无
 * @note 必须与 hal_console_lock 同核配对
 * @warning 未持锁时调用行为未定义
 * @attention ❌ ISR 勿误释放；❌ block/switch
 */
void hal_console_unlock(void);

/* -------------------------------------------------------------------------- */
/* Timer                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化本 hart 系统定时器
 * @details 查找 TIMER 设备；tick_hz==0 则用 CONFIG_TICK_RATE_HZ；调用 ops->init。
 * @param[in] tick_hz 期望 tick 频率（Hz）；0 表示使用 CONFIG_TICK_RATE_HZ
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 初始化成功
 * @retval HAL_ERR_NODEV 无设备或无 init
 * @note boot 由 hal_board_init 调用
 * @warning 重复 init 行为取决于驱动
 * @attention ❌ ISR；❌ block/switch
 */
hal_status_t hal_timer_init(uint32_t tick_hz);

/**
 * @brief 读 mtime 自由运行计数
 * @details 查找 TIMER 设备并调用 ops->mtime_read；无设备返回 0。
 * @return 64 位 mtime 值；不可用时 0
 * @retval uint64_t 当前 mtime
 * @retval 0 无 TIMER 设备或未实现 mtime_read
 * @note 无锁只读，ISR / 多核安全
 * @warning 0 可能是合法计数低位，需结合上下文判断
 * @attention ✅ ISR；❌ 不阻塞
 */
uint64_t hal_mtime_read(void);

/* -------------------------------------------------------------------------- */
/* IRQC                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化本 hart IRQC
 * @details 步骤：1. 查找 IRQC 设备；2. 调用 ops->init；3. 置 READY。
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 初始化成功
 * @retval HAL_ERR_NODEV 无设备或无 init
 * @note boot 由 hal_board_init 调用
 * @warning 重复 init 行为取决于驱动
 * @attention ❌ ISR；❌ block/switch
 */
hal_status_t hal_irqc_init(void);

/**
 * @brief claim 最高优先级 pending 源
 * @details 步骤：1. 取设备；2. ops->claim；3. 返回。
 * @return 源 ID；0=无
 * @retval >0 已 claim 的中断源号
 * @retval 0 无 pending 或无设备
 * @note trap 入口应直调驱动，勿为图省事再调用本 API
 * @warning complete 前勿重复 claim 同一源
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t hal_irqc_claim(void);

/**
 * @brief 完成中断处理并写 complete
 * @details 步骤：1. 取设备；2. ops->complete(dev, irq)。
 * @param[in] irq 先前 claim 的源号
 * @return 无
 * @retval 无
 * @note 须与 hal_irqc_claim 配对
 * @warning irq 须为最近一次 claim 的源
 * @attention ✅ ISR；❌ 不阻塞
 */
void hal_irqc_complete(uint32_t irq);

/**
 * @brief 设置本 hart 优先级阈值
 * @details 步骤：1. 取设备；2. ops->set_threshold。
 * @param[in] threshold 新阈值；0 允许全部 priority>0
 * @return 无
 * @retval 无
 * @note 每 hart 独立，无全局锁
 * @warning 无设备时静默忽略
 * @attention ✅ ISR；❌ 不阻塞
 */
void hal_irqc_set_threshold(uint32_t threshold);

/**
 * @brief 读本 hart 优先级阈值
 * @details 步骤：1. 取设备；2. ops->get_threshold。
 * @return 当前 threshold；无设备时 0
 * @retval uint32_t 当前阈值
 * @retval 0 无设备或未实现
 * @note 只读
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t hal_irqc_get_threshold(void);

/**
 * @brief 设置中断源优先级
 * @details 步骤：1. 校验设备；2. 取配置锁；3. ops->set_priority；4. 放锁。
 * @param[in] irq 源号
 * @param[in] priority 优先级 0..CONFIG_IRQ_PRIORITY_MAX
 * @return HAL_OK / HAL_ERR_PARAM / HAL_ERR_NODEV
 * @retval HAL_OK 设置成功
 * @retval HAL_ERR_PARAM 参数非法（由驱动判定）
 * @retval HAL_ERR_NODEV 无设备或无 set_priority
 * @note 配置锁保护 RMW；建议 boot/任务上下文
 * @warning 持锁期间禁止 yield；勿在 ISR 改表
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_irqc_set_priority(uint32_t irq, uint32_t priority);

/**
 * @brief 读中断源优先级
 * @details 步骤：1. 取设备；2. ops->get_priority。
 * @param[in] irq 源号
 * @return 优先级；非法/无设备为 0
 * @retval uint32_t 当前优先级
 * @retval 0 无设备、未实现或非法 irq
 * @note 只读，无配置锁
 * @warning 非法 irq 行为取决于驱动
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t hal_irqc_get_priority(uint32_t irq);

/**
 * @brief 对本 hart 使能中断源
 * @details 步骤：1. 取配置锁；2. ops->enable；3. 放锁。
 * @param[in] irq 源号
 * @return HAL_OK / HAL_ERR_PARAM / HAL_ERR_NODEV
 * @retval HAL_OK 使能成功
 * @retval HAL_ERR_PARAM 参数非法（由驱动判定）
 * @retval HAL_ERR_NODEV 无设备或无 enable
 * @note per-hart enable RMW
 * @warning 持锁期间禁止 yield；勿在 ISR 改表
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_irqc_enable(uint32_t irq);

/**
 * @brief 对本 hart 禁用中断源
 * @details 步骤：1. 取配置锁；2. ops->disable；3. 放锁。
 * @param[in] irq 源号
 * @return HAL_OK / HAL_ERR_PARAM / HAL_ERR_NODEV
 * @retval HAL_OK 禁用成功
 * @retval HAL_ERR_PARAM 参数非法（由驱动判定）
 * @retval HAL_ERR_NODEV 无设备或无 disable
 * @note per-hart disable RMW
 * @warning 持锁期间禁止 yield；勿在 ISR 改表
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_irqc_disable(uint32_t irq);

/* -------------------------------------------------------------------------- */
/* IPI / CPU                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief 向目标 hart 发送软件 IPI
 * @details
 * 1. 查找 IPI 设备；未注册返回 HAL_ERR_NODEV。
 * 2. 调用 ops->send 写目标 MSIP=1。
 * @param[in] hart 目标 hart 编号
 * @return HAL_OK / HAL_ERR_PARAM / HAL_ERR_NODEV
 * @retval HAL_OK        成功
 * @retval HAL_ERR_PARAM hart 越界
 * @retval HAL_ERR_NODEV 设备未注册
 * @note 底层实现见 arch 侧 IPI 驱动
 * @warning 目标 hart 须已上线
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_status_t hal_ipi_send(uint8_t hart);

/**
 * @brief 清目标 hart 的 MSIP 挂起位
 * @details
 * 1. 查找 IPI 设备；未注册则忽略。
 * 2. 调用 ops->clear 写 MSIP=0。
 * @param[in] hart 目标 hart 编号
 * @return 无
 * @retval 无
 * @note 通常在本核 IPI ISR 中清本核
 * @warning hart 越界时底层行为未定义
 * @attention ✅ ISR；❌ 不阻塞
 */
void hal_ipi_clear(uint8_t hart);

/**
 * @brief 早期打开 MSIE|MTIE|MEIE 三类 M 模式中断使能
 * @details
 * 1. 查找 CPU 设备；未注册返回 HAL_ERR_NODEV。
 * 2. 调用 ops->init 置 mie 相应位。
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK        成功
 * @retval HAL_ERR_NODEV 设备未注册
 * @note 底层实现见 arch 侧；hal_board_init 按序调用
 * @warning 须在 trap 向量就绪后调用
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_cpu_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CGRTOS_HAL_H */
