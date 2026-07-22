/**
 * @file hal.h
 * @brief CG-RTOS 统一外设驱动框架与用户 HAL API
 *
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
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

#define HAL_OK           0    /**< 成功 */
#define HAL_ERR_PARAM  (-1)   /**< 参数非法 */
#define HAL_ERR_NODEV  (-2)   /**< 设备未注册或未就绪 */
#define HAL_ERR_IO     (-3)   /**< 硬件 I/O 失败（预留） */
#define HAL_ERR_BUSY   (-4)   /**< 资源忙（预留） */
#define HAL_ERR_STATE  (-5)   /**< 状态不允许（如冻结后再注册） */

/* -------------------------------------------------------------------------- */
/* Device framework                                                            */
/* -------------------------------------------------------------------------- */

/** @brief 设备类别（每板通常每类一个实例；find 返回首个匹配） */
typedef enum {
    HAL_DEV_CPU     = 0, /**< CPU / mie 早期配置 */
    HAL_DEV_CONSOLE = 1, /**< 调试控制台 UART */
    HAL_DEV_TIMER   = 2, /**< 系统节拍定时器（mtime/mtimecmp） */
    HAL_DEV_IRQC    = 3, /**< 外部中断控制器（PLIC） */
    HAL_DEV_IPI     = 4, /**< 核间软件中断（MSIP） */
    HAL_DEV_CLASS_MAX
} hal_dev_class_t;

#define HAL_DEV_F_READY   0x01U  /**< 设备已完成硬件 init */
#define HAL_DEV_F_REG     0x02U  /**< 设备已进入注册表 */

struct hal_device;

/**
 * @brief 控制台驱动 ops
 * @note putc/pollc 由 HAL 在锁内调用；驱动实现不必再加锁。
 */
typedef struct {
    /** @brief 硬件初始化；返回 HAL_OK 或负错误码 */
    hal_status_t (*init)(struct hal_device *dev);
    /** @brief 阻塞发送一字符；建议对 '\\n' 追加 '\\r' */
    void (*putc)(struct hal_device *dev, char c);
    /**
     * @brief 非阻塞接收
     * @return 0..255 数据；-1 表示 RX 空（勿与 HAL_ERR_* 混淆）
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
     * @param tick_hz 期望频率；板级可忽略并用 CONFIG_TICK_RATE_HZ
     */
    hal_status_t (*init)(struct hal_device *dev, uint32_t tick_hz);
    /** @brief 读自由运行时间计数器 */
    uint64_t (*mtime_read)(struct hal_device *dev);
} hal_timer_ops_t;

/**
 * @brief 外部中断控制器 ops
 * @note claim/complete/threshold 面向当前 hart；enable/priority 含跨源共享寄存器。
 */
typedef struct {
    hal_status_t (*init)(struct hal_device *dev);
    uint32_t (*claim)(struct hal_device *dev);
    void (*complete)(struct hal_device *dev, uint32_t irq);
    void (*set_threshold)(struct hal_device *dev, uint32_t thr);
    uint32_t (*get_threshold)(struct hal_device *dev);
   hal_status_t (*set_priority)(struct hal_device *dev, uint32_t irq, uint32_t prio);
    uint32_t (*get_priority)(struct hal_device *dev, uint32_t irq);
   hal_status_t (*enable)(struct hal_device *dev, uint32_t irq);
   hal_status_t (*disable)(struct hal_device *dev, uint32_t irq);
} hal_irqc_ops_t;

/** @brief IPI ops */
typedef struct {
   hal_status_t (*init)(struct hal_device *dev);
    /**
     * @brief 向目标 hart 写 MSIP=1
     * @return HAL_OK / HAL_ERR_PARAM（hart 越界）
     */
   hal_status_t (*send)(struct hal_device *dev, uint8_t hart);
    /** @brief 清目标 hart MSIP（通常清本核） */
    void (*clear)(struct hal_device *dev, uint8_t hart);
} hal_ipi_ops_t;

/** @brief CPU 早期初始化 ops */
typedef struct {
   hal_status_t (*init)(struct hal_device *dev);
} hal_cpu_ops_t;

/**
 * @brief 统一设备描述符（静态分配，注册后生命周期 = 系统寿命）
 */
typedef struct hal_device {
    const char *name;           /**< 唯一短名，如 "uart0" */
   hal_dev_class_t class;      /**< 类别 */
    uintptr_t mmio_base;        /**< 主 MMIO 基址（诊断 / 移植核对） */
    const void *ops;            /**< 类别专属 ops，不可为 NULL */
    void *priv;                 /**< 驱动私有；可为 NULL */
    uint8_t flags;              /**< HAL_DEV_F_* */
} hal_device_t;

#ifndef HAL_DEVICE_MAX
#define HAL_DEVICE_MAX  8
#endif

#ifndef HAL_CONSOLE_PRINTF_BUF
#define HAL_CONSOLE_PRINTF_BUF  256
#endif

/**
 * @brief 注册设备（仅 boot / hal_board_init 完成前）
 * @param dev 静态对象指针
 * @return HAL_OK；参数错 HAL_ERR_PARAM；表满或冻结 HAL_ERR_STATE；同名已存在视为 HAL_OK
 * @threadsafe 内部关本核中断 + 注册自旋锁；仍应只在 hart0 启动路径调用
 * @attention ❌ ISR；❌ 运行期禁止
 */
hal_status_t hal_device_register(hal_device_t *dev);

/**
 * @brief 按类别查找（返回第一个）
 * @return 设备指针；未找到 NULL
 * @threadsafe 注册表冻结后只读，可无锁并发读
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
hal_device_t *hal_device_find(hal_dev_class_t cls);

/**
 * @brief 按名称查找
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
hal_device_t *hal_device_find_by_name(const char *name);

/**
 * @brief 已注册设备数量
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int hal_device_count(void);

/**
 * @brief 按索引取设备（0 .. count-1）；越界 NULL
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
hal_device_t *hal_device_get(int index);

/**
 * @brief 查询注册表是否已冻结
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int hal_registry_frozen(void);

/**
 * @brief 板级一键初始化：注册、按序 init、冻结注册表
 * @return HAL_OK 或首个失败码（尽量继续注册/初始化其余设备）
 *
 * @details 步骤（幂等；已初始化则直接返回 HAL_OK）：
 * 1. 调用各 `drv_*_device()` 取得静态设备描述符。
 * 2. `hal_device_register` 写入注册表（驱动本身不注册）。
 * 3. 按序 init：CPU → Console → IRQC → Timer → IPI。
 * 4. 置 `g_registry_frozen`，禁止运行期再注册。
 *
 * @threadsafe 仅 hart0 启动路径调用一次
 * @note 此函数是 HAL 调用驱动的入口；trap 路径不得反向调用本函数。
 * @attention ❌ ISR；❌ 不阻塞（boot 一次性）
 */
hal_status_t hal_board_init(void);


/* -------------------------------------------------------------------------- */
/* Console                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化控制台硬件
 * @return HAL_OK / HAL_ERR_NODEV
 * @attention ❌ ISR；❌ boot/任务上下文
 */
hal_status_t hal_console_init(void);

/**
 * @brief 输出一字符（自动加锁）
 * @threadsafe SMP 安全（逐字符）；整行请用 puts/write
 * @attention ✅ ISR 可用；⚠️ 勿长时间占锁
 */
void hal_console_putc(char c);

/**
 * @brief 已持锁时输出一字符
 * @warning 未持锁调用会导致数据竞争
 * @attention ✅ ISR 可用（须已持锁）；❌ 勿嵌套取锁
 */
void hal_console_putc_unlocked(char c);

/**
 * @brief 非阻塞读一字符
 * @return 0..255；RX 空或未就绪返回 -1
 * @attention ✅ ISR；❌ 不阻塞
 */
int hal_console_pollc(void);

/**
 * @brief 阻塞读一字符；调度运行后空闲时 yield
 * @note 等待期间不持控制台锁
 * @attention ❌ ISR；✅ 可能阻塞/yield
 */
char hal_console_getc(void);

/**
 * @brief 输出 NUL 字符串（整串持锁，行级原子）
 * @attention ✅ ISR 可用；⚠️ 勿长时间占锁
 */
void hal_console_puts(const char *s);

/**
 * @brief 输出原始缓冲区（整段持锁）
 * @return 写出字节数；失败负状态码
 * @attention ✅ ISR 可用；⚠️ 勿长时间占锁
 */
int hal_console_write(const void *buf, size_t len);

/**
 * @brief 获取控制台输出锁（不可重入）
 * @attention ❌ ISR 勿与任务争锁；❌ 不可重入
 */
void hal_console_lock(void);

/**
 * @brief 释放控制台输出锁
 * @attention ❌ ISR 勿误释放；须与 lock 配对
 */
void hal_console_unlock(void);

/* -------------------------------------------------------------------------- */
/* Timer                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化本 hart 系统定时器
 * @param tick_hz 期望 tick；0 表示使用 CONFIG_TICK_RATE_HZ
 * @attention ❌ ISR；❌ boot/任务上下文
 */
hal_status_t hal_timer_init(uint32_t tick_hz);

/**
 * @brief 读 mtime
 * @threadsafe ISR / 多核安全（只读）
 * @attention ✅ ISR；❌ 不阻塞
 */
uint64_t hal_mtime_read(void);

/* -------------------------------------------------------------------------- */
/* IRQC                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化本 hart IRQC
 * @return HAL_OK / HAL_ERR_NODEV
 * @details 步骤：1. 查找 IRQC 设备；2. 调用 ops->init；3. 置 READY。
 * @attention ❌ ISR；❌ boot/任务上下文
 */
hal_status_t hal_irqc_init(void);

/**
 * @brief claim 最高优先级 pending 源
 * @return 源 ID；0=无
 * @threadsafe 对本 hart；ISR 路径无全局锁
 * @details 步骤：1. 取设备；2. ops->claim；3. 返回。
 * @note trap 入口应直调驱动，勿为图省事再调用本 API。
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t hal_irqc_claim(void);

/**
 * @brief 完成中断处理并写 complete
 * @param irq 先前 claim 的源号
 * @details 步骤：1. 取设备；2. ops->complete(dev, irq)。
 * @attention ✅ ISR；❌ 不阻塞
 */
void hal_irqc_complete(uint32_t irq);

/**
 * @brief 设置本 hart 优先级阈值
 * @param threshold 新阈值；0 允许全部 priority>0
 * @details 步骤：1. 取设备；2. ops->set_threshold。
 * @attention ✅ ISR；❌ 不阻塞
 */
void hal_irqc_set_threshold(uint32_t threshold);

/**
 * @brief 读本 hart 优先级阈值
 * @return 当前 threshold；无设备时 0
 * @details 步骤：1. 取设备；2. ops->get_threshold。
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t hal_irqc_get_threshold(void);

/**
 * @brief 设置中断源优先级
 * @param irq 源号；@param priority 0..CONFIG_IRQ_PRIORITY_MAX
 * @return HAL_OK / HAL_ERR_PARAM / HAL_ERR_NODEV
 * @threadsafe 配置锁保护 RMW；建议 boot/任务上下文
 * @details 步骤：1. 校验；2. 取配置锁；3. ops->set_priority；4. 放锁。
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_irqc_set_priority(uint32_t irq, uint32_t priority);

/**
 * @brief 读中断源优先级
 * @param irq 源号
 * @return 优先级；非法/无设备为 0
 * @details 步骤：1. 取设备；2. ops->get_priority。
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
uint32_t hal_irqc_get_priority(uint32_t irq);

/**
 * @brief 对本 hart 使能中断源
 * @param irq 源号
 * @return HAL_OK / HAL_ERR_PARAM / HAL_ERR_NODEV
 * @details 步骤：1. 取配置锁；2. ops->enable；3. 放锁。
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_irqc_enable(uint32_t irq);

/**
 * @brief 对本 hart 禁用中断源
 * @param irq 源号
 * @return HAL_OK / HAL_ERR_PARAM / HAL_ERR_NODEV
 * @details 步骤：1. 取配置锁；2. ops->disable；3. 放锁。
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
 * @note 底层实现见 arch/riscv/ipic.c
 * @warning 目标 hart 须已上线
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_ipi_send(uint8_t hart);

/**
 * @brief 清目标 hart 的 MSIP 挂起位
 * @details
 * 1. 查找 IPI 设备；未注册则忽略。
 * 2. 调用 ops->clear 写 MSIP=0。
 * @param[in] hart 目标 hart 编号
 * @return 无
 * @note 通常在本核 IPI ISR 中清本核；底层见 arch/riscv/ipic.c
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
 * @note 底层实现见 arch/riscv/arch.c；hal_board_init 按序调用
 * @warning 须在 trap 向量就绪后调用
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_cpu_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CGRTOS_HAL_H */
