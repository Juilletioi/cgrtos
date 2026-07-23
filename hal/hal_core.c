/**
 * @file hal_core.c
 * @brief HAL 设备注册表、锁与用户 API 分发实现
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * ## 依赖方向（禁止倒置）
 * @code
 *   应用 / 内核业务  →  本文件 hal_*  →  drv_*_device()->ops  →  MMIO
 *   Trap / 底层 ISR  →  驱动 static *_hw_*（禁止再进本文件）
 * @endcode
 *
 * ## 注册表生命周期
 * 1. Boot：`hal_board_init` 经 `drv_*_device()` 取得描述符并 `hal_device_register`。
 * 2. 按依赖顺序调用各类 `hal_*_init`，缓存类别指针。
 * 3. 冻结注册表；运行期 find 只读，再 register 返回 HAL_ERR_STATE。
 *
 * ## 锁策略（刻意不用 g_klock）
 * - g_reg_lock：保护注册表写（boot 短窗口）。
 * - g_console_lock + 本核 irq_save：串行化 UART 输出，避免多核字节交错；
 *   与内核临界区解耦，防止 printf 嵌套 enter_critical 死锁。
 * - g_irqc_cfg_lock + irq_save：保护 PLIC priority/enable 的 RMW。
 * - claim/complete/threshold/mtime：无全局锁（每 hart 或只读）。
 */

#include "hal.h"
#include "hal_board.h"
#include "hal_drv.h"
#include "../kernel/cgrtos.h"

static hal_device_t *g_devs[HAL_DEVICE_MAX];
static int g_dev_count;
static volatile uint8_t g_registry_frozen;
static volatile uint8_t g_board_inited;

/** 每类缓存，避免热路径反复线性查找 */
static hal_device_t *g_by_class[HAL_DEV_CLASS_MAX];

static spinlock_t g_reg_lock;
static spinlock_t g_console_lock;
static spinlock_t g_irqc_cfg_lock;

/* 控制台锁持有者保存的 mstatus（每核一份，支持不同核交错 lock/unlock 配对） */
static uint64_t g_console_irq_saved[CONFIG_MAX_CORES];
static uint8_t g_console_lock_nest[CONFIG_MAX_CORES]; /* 仅诊断：应为 0/1 */

static uint64_t g_irqc_irq_saved[CONFIG_MAX_CORES];

/**
 * @brief 比较两 C 字符串是否完全相等
 * @details 逐字节比较直至双 NUL；任一指针为 NULL 则视为不等。
 * @param[in] a 首串
 * @param[in] b 次串
 * @return 相等为 1，否则 0
 * @retval 1 内容一致
 * @retval 0 不等或指针无效
 * @note 区分大小写
 * @warning 指针须有效或同为 NULL
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static int str_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/**
 * @brief 将设备写入按类别缓存表（首注册 wins）
 * @details 若 dev 合法且 g_by_class[class] 为空，则缓存指针以加速热路径查找。
 * @param[in] dev 设备描述符；NULL 或 class 越界时直接返回
 * @return 无
 * @retval 无
 * @note 幂等：同类已有缓存则跳过
 * @warning 须在注册表写入路径调用
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static void cache_device(hal_device_t *dev)
{
    if (!dev || (unsigned)dev->class >= (unsigned)HAL_DEV_CLASS_MAX) {
        return;
    }
    if (!g_by_class[dev->class]) {
        g_by_class[dev->class] = dev;
    }
}

/**
 * @brief 取得 CONSOLE 类别设备（缓存优先）
 * @details 先读 g_by_class[HAL_DEV_CONSOLE]；未命中则 hal_device_find 线性查找。
 * @return 设备指针；未注册时为 NULL
 * @retval 非 NULL 已注册控制台设备
 * @retval NULL 无 CONSOLE 设备
 * @note 热路径 helper
 * @warning 返回指针寿命为静态描述符
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static hal_device_t *dev_console(void)
{
    return g_by_class[HAL_DEV_CONSOLE]
               ? g_by_class[HAL_DEV_CONSOLE]
               : hal_device_find(HAL_DEV_CONSOLE);
}

/**
 * @brief 取得 TIMER 类别设备（缓存优先）
 * @details 先读 g_by_class[HAL_DEV_TIMER]；未命中则 hal_device_find 线性查找。
 * @return 设备指针；未注册时为 NULL
 * @retval 非 NULL 已注册定时器设备
 * @retval NULL 无 TIMER 设备
 * @note 热路径 helper
 * @warning 返回指针寿命为静态描述符
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static hal_device_t *dev_timer(void)
{
    return g_by_class[HAL_DEV_TIMER]
               ? g_by_class[HAL_DEV_TIMER]
               : hal_device_find(HAL_DEV_TIMER);
}

/**
 * @brief 取得 IRQC 类别设备（缓存优先）
 * @details 先读 g_by_class[HAL_DEV_IRQC]；未命中则 hal_device_find 线性查找。
 * @return 设备指针；未注册时为 NULL
 * @retval 非 NULL 已注册中断控制器设备
 * @retval NULL 无 IRQC 设备
 * @note 热路径 helper
 * @warning 返回指针寿命为静态描述符
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static hal_device_t *dev_irqc(void)
{
    return g_by_class[HAL_DEV_IRQC]
               ? g_by_class[HAL_DEV_IRQC]
               : hal_device_find(HAL_DEV_IRQC);
}

/**
 * @brief 取得 IPI 类别设备（缓存优先）
 * @details 先读 g_by_class[HAL_DEV_IPI]；未命中则 hal_device_find 线性查找。
 * @return 设备指针；未注册时为 NULL
 * @retval 非 NULL 已注册 IPI 设备
 * @retval NULL 无 IPI 设备
 * @note 热路径 helper
 * @warning 返回指针寿命为静态描述符
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static hal_device_t *dev_ipi(void)
{
    return g_by_class[HAL_DEV_IPI]
               ? g_by_class[HAL_DEV_IPI]
               : hal_device_find(HAL_DEV_IPI);
}

/**
 * @brief 取得 CPU 类别设备（缓存优先）
 * @details 先读 g_by_class[HAL_DEV_CPU]；未命中则 hal_device_find 线性查找。
 * @return 设备指针；未注册时为 NULL
 * @retval 非 NULL 已注册 CPU 设备
 * @retval NULL 无 CPU 设备
 * @note 热路径 helper
 * @warning 返回指针寿命为静态描述符
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static hal_device_t *dev_cpu(void)
{
    return g_by_class[HAL_DEV_CPU]
               ? g_by_class[HAL_DEV_CPU]
               : hal_device_find(HAL_DEV_CPU);
}

/* -------------------------------------------------------------------------- */
/* Registry                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 注册设备到全局表（仅 boot，冻结前）
 * @details 步骤：
 * 1. 校验 dev / name / ops / class 范围。
 * 2. 关本核中断并取 g_reg_lock。
 * 3. 若已冻结 → 解锁恢复中断，返回 HAL_ERR_STATE。
 * 4. 同名已存在 → 视为幂等成功。
 * 5. 表满 → HAL_ERR_STATE。
 * 6. 写入 g_devs[]，置 HAL_DEV_F_REG，cache_device。
 * 7. 释放锁并恢复中断。
 * @param[in] dev 静态寿命的设备描述符；name/ops/class 必填
 * @return HAL_OK；参数错 HAL_ERR_PARAM；满表或已冻结 HAL_ERR_STATE
 * @retval HAL_OK 注册成功或同名幂等
 * @retval HAL_ERR_PARAM 参数非法
 * @retval HAL_ERR_STATE 表满或注册表已冻结
 * @note 驱动文件不得调用本函数；由 hal_board_init 统一调用。
 * @warning 冻结后调用恒返回 HAL_ERR_STATE
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_device_register(hal_device_t *dev)
{
    uint64_t flags;
    int i;

    if (!dev || !dev->name || !dev->ops) {
        return HAL_ERR_PARAM;
    }
    if ((unsigned)dev->class >= (unsigned)HAL_DEV_CLASS_MAX) {
        return HAL_ERR_PARAM;
    }

    flags = cgrtos_irq_save();
    cgrtos_spin_lock(&g_reg_lock);

    if (g_registry_frozen) {
        cgrtos_spin_unlock(&g_reg_lock);
        cgrtos_irq_restore(flags);
        return HAL_ERR_STATE;
    }

    for (i = 0; i < g_dev_count; i++) {
        if (g_devs[i] && str_eq(g_devs[i]->name, dev->name)) {
            cache_device(g_devs[i]);
            cgrtos_spin_unlock(&g_reg_lock);
            cgrtos_irq_restore(flags);
            return HAL_OK; /* 幂等 */
        }
    }
    if (g_dev_count >= HAL_DEVICE_MAX) {
        cgrtos_spin_unlock(&g_reg_lock);
        cgrtos_irq_restore(flags);
        return HAL_ERR_STATE;
    }

    g_devs[g_dev_count++] = dev;
    dev->flags |= HAL_DEV_F_REG;
    cache_device(dev);
    __sync_synchronize();

    cgrtos_spin_unlock(&g_reg_lock);
    cgrtos_irq_restore(flags);
    return HAL_OK;
}

/**
 * @brief 按设备类别查找已注册设备
 * @details 先查 g_by_class 缓存；未命中则遍历 g_devs[] 按 class 匹配。
 * @param[in] cls 设备类别枚举
 * @return 匹配的设备指针；未找到为 NULL
 * @retval 非 NULL 首个匹配该类别的设备
 * @retval NULL 无此类别或 cls 越界
 * @note 冻结前后均可只读调用
 * @warning 返回指针为静态描述符，勿 free
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *hal_device_find(hal_dev_class_t cls)
{
    int i;
    if ((unsigned)cls < (unsigned)HAL_DEV_CLASS_MAX && g_by_class[cls]) {
        return g_by_class[cls];
    }
    for (i = 0; i < g_dev_count; i++) {
        if (g_devs[i] && g_devs[i]->class == cls) {
            return g_devs[i];
        }
    }
    return 0;
}

/**
 * @brief 按名称查找已注册设备
 * @details 线性扫描 g_devs[]，str_eq 比较 name 字段。
 * @param[in] name 设备名字符串；NULL 时直接返回 NULL
 * @return 匹配的设备指针；未找到为 NULL
 * @retval 非 NULL 同名设备
 * @retval NULL 未找到或 name 为 NULL
 * @note 冻结前后均可只读调用
 * @warning 返回指针为静态描述符，勿 free
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *hal_device_find_by_name(const char *name)
{
    int i;
    if (!name) {
        return 0;
    }
    for (i = 0; i < g_dev_count; i++) {
        if (g_devs[i] && str_eq(g_devs[i]->name, name)) {
            return g_devs[i];
        }
    }
    return 0;
}

/**
 * @brief 返回当前已注册设备数量
 * @details 只读返回 g_dev_count。
 * @return 已注册条目数（0..HAL_DEVICE_MAX）
 * @retval >=0 当前表内设备数
 * @note 冻结前后均可调用
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int hal_device_count(void)
{
    return g_dev_count;
}

/**
 * @brief 按索引取得注册表中的设备
 * @details 边界检查后返回 g_devs[index]。
 * @param[in] index 从 0 起的表下标
 * @return 设备指针；越界为 NULL
 * @retval 非 NULL 对应槽位设备
 * @retval NULL index 非法
 * @note 用于枚举/调试
 * @warning 返回指针为静态描述符
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_device_t *hal_device_get(int index)
{
    if (index < 0 || index >= g_dev_count) {
        return 0;
    }
    return g_devs[index];
}

/**
 * @brief 查询注册表是否已冻结
 * @details 只读检查 g_registry_frozen 非零。
 * @return 非 0 表示已冻结；0 表示仍可注册
 * @retval 1 已冻结
 * @retval 0 未冻结
 * @note hal_board_init 末尾置位
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
int hal_registry_frozen(void)
{
    return g_registry_frozen != 0;
}

/**
 * @brief 板级 HAL 一次性初始化入口
 * @details 步骤：
 * 1. 幂等：若 g_board_inited 已置位则直接 HAL_OK。
 * 2. drv_*_device() 取得静态描述符并 hal_device_register（单失败不中止）。
 * 3. 按依赖顺序 init：CPU → Console → IRQC → Timer → IPI。
 * 4. 内存屏障后冻结注册表并置 g_board_inited。
 * @return 首个失败状态或 HAL_OK
 * @retval HAL_OK 全部成功或已初始化
 * @retval HAL_ERR_* 任一 register/init 失败（保留首个错误）
 * @note 仅 boot 调用一次；驱动不得反向调用 HAL。
 * @warning 重复调用安全但不再注册新设备
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_board_init(void)
{
    hal_status_t st = HAL_OK;
    hal_status_t r;

    if (g_board_inited) {
        return HAL_OK;
    }

    /*
     * 1. 注册板级设备（依赖方向：HAL 调 drv_*，驱动不调 HAL）
     *    步骤：
     *    a. drv_*_device() 取得静态描述符（驱动侧只导出，不注册）。
     *    b. hal_device_register 写入注册表并做参数校验。
     *    c. 单个失败不中止，尽量凑齐其余设备。
     */
    {
    hal_device_t *list[] = {
            drv_cpu_device(),
            drv_uart_device(),
            drv_irqc_device(),
            drv_timer_device(),
            drv_ipi_device(),
        };
        unsigned i;
        for (i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
            r = hal_device_register(list[i]);
            if (r != HAL_OK && st == HAL_OK) {
                st = r;
            }
        }
    }

    /* 2. 按依赖顺序 init：CPU → Console → IRQC → Timer → IPI */
    r = hal_cpu_init();
    if (r != HAL_OK && st == HAL_OK) {
        st = r;
    }
    r = hal_console_init();
    if (r != HAL_OK && st == HAL_OK) {
        st = r;
    }
    r = hal_irqc_init();
    if (r != HAL_OK && st == HAL_OK) {
        st = r;
    }
    r = hal_timer_init(CONFIG_TICK_RATE_HZ);
    if (r != HAL_OK && st == HAL_OK) {
        st = r;
    }
    /* IPI：register 即可；可选 init */
    {
    hal_device_t *d = dev_ipi();
        if (d) {
            const hal_ipi_ops_t *ops = (const hal_ipi_ops_t *)d->ops;
            if (ops && ops->init) {
                r = ops->init(d);
                if (r == HAL_OK) {
                    d->flags |= HAL_DEV_F_READY;
                } else if (st == HAL_OK) {
                    st = r;
                }
            } else {
                d->flags |= HAL_DEV_F_READY;
            }
            cache_device(d);
        }
    }

    __sync_synchronize();
    g_registry_frozen = 1;
    g_board_inited = 1;
    __sync_synchronize();
    return st;
}

/* -------------------------------------------------------------------------- */
/* Console lock                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 取得控制台输出权（不可重入）
 * @details 步骤：
 * 1. 读 mhartid，越界则按 0。
 * 2. cgrtos_irq_save 关本核中断，保存到 g_console_irq_saved[cpu]。
 * 3. 自旋获取 g_console_lock（非 g_klock）。
 * 4. 标记 nest=1（诊断用）。
 * @return 无
 * @retval 无
 * @note 与 hal_console_unlock 成对调用
 * @warning 持锁期间禁止 yield / 再取 g_klock / 长时间阻塞
 * @attention ❌ ISR；❌ 不阻塞
 */
void hal_console_lock(void)
{
    uint8_t cpu = arch_cpu_id();
    if (cpu >= CONFIG_MAX_CORES) {
        cpu = 0;
    }
    g_console_irq_saved[cpu] = cgrtos_irq_save();
    cgrtos_spin_lock(&g_console_lock);
    g_console_lock_nest[cpu] = 1;
}

/**
 * @brief 释放控制台输出权
 * @details 步骤：
 * 1. 读 mhartid（同 lock 规则）。
 * 2. 清 nest 标记。
 * 3. 释放 g_console_lock。
 * 4. cgrtos_irq_restore 恢复进入时的 mstatus。
 * @return 无
 * @retval 无
 * @note 必须与 hal_console_lock 同核配对
 * @warning 未持锁时调用行为未定义
 * @attention ❌ ISR；❌ 不阻塞
 */
void hal_console_unlock(void)
{
    uint8_t cpu = arch_cpu_id();
    if (cpu >= CONFIG_MAX_CORES) {
        cpu = 0;
    }
    g_console_lock_nest[cpu] = 0;
    cgrtos_spin_unlock(&g_console_lock);
    cgrtos_irq_restore(g_console_irq_saved[cpu]);
}

/**
 * @brief 取得 IRQC 配置锁并关本核中断
 * @details 步骤：1. 读 mhartid；2. irq_save 保存 flags；3. 自旋 g_irqc_cfg_lock。
 * @param[out] cpu_out   当前 hart id（越界归 0）
 * @param[out] flags_out 进入时的 mstatus 快照
 * @return 无
 * @retval 无
 * @note 与 irqc_cfg_unlock 成对
 * @warning 持锁期间禁止 yield
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
 */
static void irqc_cfg_lock(uint8_t *cpu_out, uint64_t *flags_out)
{
    uint8_t cpu = arch_cpu_id();
    if (cpu >= CONFIG_MAX_CORES) {
        cpu = 0;
    }
    *cpu_out = cpu;
    *flags_out = cgrtos_irq_save();
    cgrtos_spin_lock(&g_irqc_cfg_lock);
    g_irqc_irq_saved[cpu] = *flags_out;
}

/**
 * @brief 释放 IRQC 配置锁并恢复中断
 * @details 步骤：1. 释放 g_irqc_cfg_lock；2. irq_restore 恢复 flags。
 * @param[in] cpu    当前 hart（未使用，保留签名一致）
 * @param[in] flags  irqc_cfg_lock 保存的 mstatus
 * @return 无
 * @retval 无
 * @note 必须与 irqc_cfg_lock 同核配对
 * @warning 未持锁时调用行为未定义
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
 */
static void irqc_cfg_unlock(uint8_t cpu, uint64_t flags)
{
    (void)cpu;
    cgrtos_spin_unlock(&g_irqc_cfg_lock);
    cgrtos_irq_restore(flags);
}

/* -------------------------------------------------------------------------- */
/* Console API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化控制台硬件
 * @details 步骤：1. 取 CONSOLE 设备；2. ops->init；3. 成功则置 READY 并缓存。
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 初始化成功
 * @retval HAL_ERR_NODEV 无设备或无 init 回调
 * @note boot 阶段由 hal_board_init 调用
 * @warning 重复 init 行为取决于驱动 ops
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_console_init(void)
{
    hal_device_t *d = dev_console();
    const hal_console_ops_t *ops;
    hal_status_t r;

    if (!d) {
        return HAL_ERR_NODEV;
    }
    ops = (const hal_console_ops_t *)d->ops;
    if (!ops || !ops->init) {
        return HAL_ERR_NODEV;
    }
    r = ops->init(d);
    if (r == HAL_OK) {
        d->flags |= HAL_DEV_F_READY;
        cache_device(d);
    }
    return r;
}

/**
 * @brief 输出一字符（调用方已持控制台锁）
 * @details 步骤：1. dev_console；2. ops->putc(d, c)；无设备或无 ops 则静默返回。
 * @param[in] c 待输出字符
 * @return 无
 * @retval 无
 * @note 供 hal_console_putc / printf 等持锁路径使用
 * @warning 调用前须已 hal_console_lock
 * @attention ✅ ISR（若调用方已关中断持锁）；❌ 不阻塞
 */
void hal_console_putc_unlocked(char c)
{
    hal_device_t *d = dev_console();
    const hal_console_ops_t *ops;
    if (!d) {
        return;
    }
    ops = (const hal_console_ops_t *)d->ops;
    if (ops && ops->putc) {
        ops->putc(d, c);
    }
}

/**
 * @brief 输出一字符（自动加锁）
 * @details 步骤：1. hal_console_lock；2. putc_unlocked；3. unlock。
 * @param[in] c 字符
 * @return 无
 * @retval 无
 * @note SMP 下整字符输出原子
 * @warning 持锁期间禁止 yield
 * @attention ❌ ISR；❌ 不阻塞
 */
void hal_console_putc(char c)
{
    hal_console_lock();
    hal_console_putc_unlocked(c);
    hal_console_unlock();
}

/**
 * @brief 非阻塞读一字符（持锁 poll）
 * @details 步骤：1. hal_console_lock；2. dev_console + ops->pollc；3. unlock；4. 返回字符或 -1。
 * @return 0..255 有数据；-1 表示 RX 空或无设备
 * @retval >=0 收到字节
 * @retval -1 无数据或设备不可用
 * @note 不 yield，仅单次 poll
 * @warning 持锁期间禁止 yield
 * @attention ❌ ISR；❌ 不阻塞
 */
int hal_console_pollc(void)
{
    hal_device_t *d;
    const hal_console_ops_t *ops;
    int ch;

    hal_console_lock();
    d = dev_console();
    if (!d) {
    hal_console_unlock();
        return -1;
    }
    ops = (const hal_console_ops_t *)d->ops;
    ch = (ops && ops->pollc) ? ops->pollc(d) : -1;
    hal_console_unlock();
    return ch;
}

/**
 * @brief 阻塞读一字符（空闲可 yield）
 * @details 步骤：循环 hal_console_pollc；无数据且 g_sched_run 时 cgrtos_task_yield，直至收到字节。
 * @return 收到的字符（低 8 位）
 * @retval char 成功读到的字符
 * @note 仅 poll 瞬间持锁；yield 前已释放控制台锁
 * @warning 调度未启动时忙等
 * @attention ❌ ISR；✅ 可能阻塞/切换（调度已启动时 yield）
 */
char hal_console_getc(void)
{
    int c;
    while ((c = hal_console_pollc()) < 0) {
        if (g_sched_run) {
            cgrtos_task_yield();
        }
    }
    return (char)c;
}

/**
 * @brief 输出 NUL 结尾字符串（整串持锁）
 * @details 步骤：1. NULL 直接返回；2. lock；3. 逐字节 putc_unlocked；4. unlock。
 * @param[in] s 字符串；NULL 安全忽略
 * @return 无
 * @retval 无
 * @note SMP 下整串不会半行交错
 * @warning 持锁期间禁止 yield
 * @attention ❌ ISR；❌ 不阻塞
 */
void hal_console_puts(const char *s)
{
    if (!s) {
        return;
    }
    hal_console_lock();
    while (*s) {
    hal_console_putc_unlocked(*s++);
    }
    hal_console_unlock();
}

/**
 * @brief 输出缓冲区若干字节（整段持锁）
 * @details 步骤：1. len==0 返回 0；2. buf NULL 返回 HAL_ERR_PARAM；3. lock 后逐字节 putc_unlocked；4. unlock。
 * @param[in] buf 数据源
 * @param[in] len 字节数
 * @return 成功写入字节数或 HAL_ERR_PARAM
 * @retval (int)len 全部写出
 * @retval 0 len 为 0
 * @retval HAL_ERR_PARAM buf 为 NULL 且 len>0
 * @note 与 hal_console_puts 相同锁策略
 * @warning 持锁期间禁止 yield
 * @attention ❌ ISR；❌ 不阻塞
 */
int hal_console_write(const void *buf, size_t len)
{
    const unsigned char *p;
    size_t i;

    if (len == 0) {
        return 0;
    }
    if (!buf) {
        return HAL_ERR_PARAM;
    }
    p = (const unsigned char *)buf;
    hal_console_lock();
    for (i = 0; i < len; i++) {
    hal_console_putc_unlocked((char)p[i]);
    }
    hal_console_unlock();
    return (int)len;
}

/* -------------------------------------------------------------------------- */
/* Timer                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化系统定时器（CLINT/mtime）
 * @details 步骤：1. dev_timer；2. tick_hz==0 则用 CONFIG_TICK_RATE_HZ；3. ops->init；4. 成功置 READY 并缓存。
 * @param[in] tick_hz 期望 tick 频率（Hz）；0 表示默认
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 初始化成功
 * @retval HAL_ERR_NODEV 无设备或无 init
 * @note boot 由 hal_board_init 调用
 * @warning 重复 init 行为取决于驱动
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_timer_init(uint32_t tick_hz)
{
    hal_device_t *d = dev_timer();
    const hal_timer_ops_t *ops;
    hal_status_t r;

    if (!d) {
        return HAL_ERR_NODEV;
    }
    ops = (const hal_timer_ops_t *)d->ops;
    if (!ops || !ops->init) {
        return HAL_ERR_NODEV;
    }
    if (tick_hz == 0) {
        tick_hz = CONFIG_TICK_RATE_HZ;
    }
    r = ops->init(d, tick_hz);
    if (r == HAL_OK) {
        d->flags |= HAL_DEV_F_READY;
        cache_device(d);
    }
    return r;
}

/**
 * @brief 读取 mtime 自由运行计数
 * @details 步骤：1. dev_timer；2. ops->mtime_read(d)；无设备或无 ops 返回 0。
 * @return 64 位 mtime 值；不可用时 0
 * @retval uint64_t 当前 mtime
 * @retval 0 无 TIMER 设备或未实现 mtime_read
 * @note 无锁只读，每 hart 安全
 * @warning 0 可能是合法计数低位，需结合上下文判断
 * @attention ✅ ISR；❌ 不阻塞
 */
uint64_t hal_mtime_read(void)
{
    hal_device_t *d = dev_timer();
    const hal_timer_ops_t *ops;
    if (!d) {
        return 0;
    }
    ops = (const hal_timer_ops_t *)d->ops;
    if (ops && ops->mtime_read) {
        return ops->mtime_read(d);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* IRQC                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化本 hart 外部中断控制器
 * @details 步骤：1. 取 IRQC 设备；2. ops->init；3. 成功则置 READY 并缓存。
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 初始化成功
 * @retval HAL_ERR_NODEV 无设备或无 init
 * @note boot 由 hal_board_init 调用
 * @warning 重复 init 行为取决于驱动
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_irqc_init(void)
{
    hal_device_t *d = dev_irqc();
    const hal_irqc_ops_t *ops;
    hal_status_t r;

    if (!d) {
        return HAL_ERR_NODEV;
    }
    ops = (const hal_irqc_ops_t *)d->ops;
    if (!ops || !ops->init) {
        return HAL_ERR_NODEV;
    }
    r = ops->init(d);
    if (r == HAL_OK) {
        d->flags |= HAL_DEV_F_READY;
        cache_device(d);
    }
    return r;
}

/**
 * @brief claim 当前 hart 最高优先级 pending 源
 * @details 步骤：1. 取 IRQC 设备；2. 调 ops->claim(dev)；3. 返回结果。
 * @return 源 ID；0 表示无 pending；无设备时 0
 * @retval >0 已 claim 的中断源号
 * @retval 0 无 pending 或无设备
 * @note 应用可用；trap 入口应直调驱动 plic_hw_claim，勿绕回本函数。
 * @warning complete 前勿重复 claim 同一源
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t hal_irqc_claim(void)
{
    hal_device_t *d = dev_irqc();
    const hal_irqc_ops_t *ops;
    if (!d) {
        return 0;
    }
    ops = (const hal_irqc_ops_t *)d->ops;
    return (ops && ops->claim) ? ops->claim(d) : 0;
}

/**
 * @brief 完成先前 claim 的中断服务
 * @details 步骤：1. dev_irqc；2. ops->complete(d, irq)；无设备则静默返回。
 * @param[in] irq 先前 claim 返回的源号
 * @return 无
 * @retval 无
 * @note 须与 hal_irqc_claim 配对
 * @warning irq 须为最近一次 claim 的源
 * @attention ✅ ISR；❌ 不阻塞
 */
void hal_irqc_complete(uint32_t irq)
{
    hal_device_t *d = dev_irqc();
    const hal_irqc_ops_t *ops;
    if (!d) {
        return;
    }
    ops = (const hal_irqc_ops_t *)d->ops;
    if (ops && ops->complete) {
        ops->complete(d, irq);
    }
}

/**
 * @brief 设置本 hart PLIC claim 阈值
 * @details 步骤：1. dev_irqc；2. ops->set_threshold(d, threshold)。
 * @param[in] threshold 新阈值
 * @return 无
 * @retval 无
 * @note 无全局锁，每 hart 独立
 * @warning 无设备时静默忽略
 * @attention ✅ ISR；❌ 不阻塞
 */
void hal_irqc_set_threshold(uint32_t threshold)
{
    hal_device_t *d = dev_irqc();
    const hal_irqc_ops_t *ops;
    if (!d) {
        return;
    }
    ops = (const hal_irqc_ops_t *)d->ops;
    if (ops && ops->set_threshold) {
        ops->set_threshold(d, threshold);
    }
}

/**
 * @brief 读取本 hart PLIC claim 阈值
 * @details 步骤：1. dev_irqc；2. ops->get_threshold(d)。
 * @return 当前阈值；无设备时 0
 * @retval uint32_t 阈值
 * @retval 0 无设备或未实现
 * @note 无锁只读
 * @warning 无
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t hal_irqc_get_threshold(void)
{
    hal_device_t *d = dev_irqc();
    const hal_irqc_ops_t *ops;
    if (!d) {
        return 0;
    }
    ops = (const hal_irqc_ops_t *)d->ops;
    return (ops && ops->get_threshold) ? ops->get_threshold(d) : 0;
}

/**
 * @brief 设置中断源优先级（持 IRQC 配置锁）
 * @details 步骤：1. dev_irqc；2. irqc_cfg_lock；3. ops->set_priority；4. unlock。
 * @param[in] irq      源编号
 * @param[in] priority 优先级
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 设置成功
 * @retval HAL_ERR_NODEV 无设备或无 set_priority
 * @note RMW 路径须串行化
 * @warning 持锁期间禁止 yield
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_irqc_set_priority(uint32_t irq, uint32_t priority)
{
    hal_device_t *d;
    const hal_irqc_ops_t *ops;
    hal_status_t r;
    uint8_t cpu;
    uint64_t flags;

    d = dev_irqc();
    if (!d) {
        return HAL_ERR_NODEV;
    }
    ops = (const hal_irqc_ops_t *)d->ops;
    if (!ops || !ops->set_priority) {
        return HAL_ERR_NODEV;
    }
    irqc_cfg_lock(&cpu, &flags);
    r = ops->set_priority(d, irq, priority);
    irqc_cfg_unlock(cpu, flags);
    return r;
}

/**
 * @brief 读取中断源优先级
 * @details 步骤：1. dev_irqc；2. ops->get_priority(d, irq)。
 * @param[in] irq 源编号
 * @return 优先级；无设备时 0
 * @retval uint32_t 当前优先级
 * @retval 0 无设备或未实现
 * @note 只读，无配置锁
 * @warning 非法 irq 行为取决于驱动
 * @attention ✅ ISR；❌ 不阻塞
 */
uint32_t hal_irqc_get_priority(uint32_t irq)
{
    hal_device_t *d = dev_irqc();
    const hal_irqc_ops_t *ops;
    if (!d) {
        return 0;
    }
    ops = (const hal_irqc_ops_t *)d->ops;
    return (ops && ops->get_priority) ? ops->get_priority(d, irq) : 0;
}

/**
 * @brief 对本 hart 使能中断源（持 IRQC 配置锁）
 * @details 步骤：1. dev_irqc；2. irqc_cfg_lock；3. ops->enable；4. unlock。
 * @param[in] irq 源编号
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 使能成功
 * @retval HAL_ERR_NODEV 无设备或无 enable
 * @note enable 为 per-hart RMW
 * @warning 持锁期间禁止 yield
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_irqc_enable(uint32_t irq)
{
    hal_device_t *d;
    const hal_irqc_ops_t *ops;
    hal_status_t r;
    uint8_t cpu;
    uint64_t flags;

    d = dev_irqc();
    if (!d) {
        return HAL_ERR_NODEV;
    }
    ops = (const hal_irqc_ops_t *)d->ops;
    if (!ops || !ops->enable) {
        return HAL_ERR_NODEV;
    }
    irqc_cfg_lock(&cpu, &flags);
    r = ops->enable(d, irq);
    irqc_cfg_unlock(cpu, flags);
    return r;
}

/**
 * @brief 对本 hart 禁用中断源（持 IRQC 配置锁）
 * @details 步骤：1. dev_irqc；2. irqc_cfg_lock；3. ops->disable；4. unlock。
 * @param[in] irq 源编号
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 禁用成功
 * @retval HAL_ERR_NODEV 无设备或无 disable
 * @note disable 为 per-hart RMW
 * @warning 持锁期间禁止 yield
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_irqc_disable(uint32_t irq)
{
    hal_device_t *d;
    const hal_irqc_ops_t *ops;
    hal_status_t r;
    uint8_t cpu;
    uint64_t flags;

    d = dev_irqc();
    if (!d) {
        return HAL_ERR_NODEV;
    }
    ops = (const hal_irqc_ops_t *)d->ops;
    if (!ops || !ops->disable) {
        return HAL_ERR_NODEV;
    }
    irqc_cfg_lock(&cpu, &flags);
    r = ops->disable(d, irq);
    irqc_cfg_unlock(cpu, flags);
    return r;
}

/* -------------------------------------------------------------------------- */
/* IPI / CPU                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief 向目标 hart 发送软件 IPI
 * @details 步骤：1. 取 IPI 设备；2. ops->send(dev, hart)；3. 返回状态。
 * @param[in] hart 目标核号
 * @return HAL_OK / HAL_ERR_PARAM / HAL_ERR_NODEV
 * @retval HAL_OK 发送成功
 * @retval HAL_ERR_NODEV 无设备或无 send
 * @retval HAL_ERR_PARAM 非法 hart（由驱动判定）
 * @note 无全局 HAL 锁
 * @warning 目标 hart 须已初始化 IPI 接收路径
 * @attention ✅ ISR；❌ 不阻塞
 */
hal_status_t hal_ipi_send(uint8_t hart)
{
    hal_device_t *d = dev_ipi();
    const hal_ipi_ops_t *ops;
    if (!d) {
        return HAL_ERR_NODEV;
    }
    ops = (const hal_ipi_ops_t *)d->ops;
    if (!ops || !ops->send) {
        return HAL_ERR_NODEV;
    }
    return ops->send(d, hart);
}

/**
 * @brief 清除目标 hart 上的软件 IPI pending
 * @details 步骤：1. dev_ipi；2. ops->clear(d, hart)；无设备则静默返回。
 * @param[in] hart 目标核号
 * @return 无
 * @retval 无
 * @note 通常在 IPI ISR 末尾调用
 * @warning 无设备时静默忽略
 * @attention ✅ ISR；❌ 不阻塞
 */
void hal_ipi_clear(uint8_t hart)
{
    hal_device_t *d = dev_ipi();
    const hal_ipi_ops_t *ops;
    if (!d) {
        return;
    }
    ops = (const hal_ipi_ops_t *)d->ops;
    if (ops && ops->clear) {
        ops->clear(d, hart);
    }
}

/**
 * @brief 初始化 CPU 抽象（打开 mie 等）
 * @details 步骤：1. dev_cpu；2. ops->init；3. 成功置 READY 并缓存。
 * @return HAL_OK / HAL_ERR_NODEV
 * @retval HAL_OK 初始化成功
 * @retval HAL_ERR_NODEV 无设备或无 init
 * @note boot 最先 init 的类别之一
 * @warning 重复 init 行为取决于驱动
 * @attention ❌ ISR；❌ 不阻塞
 */
hal_status_t hal_cpu_init(void)
{
    hal_device_t *d = dev_cpu();
    const hal_cpu_ops_t *ops;
    hal_status_t r;

    if (!d) {
        return HAL_ERR_NODEV;
    }
    ops = (const hal_cpu_ops_t *)d->ops;
    if (!ops || !ops->init) {
        return HAL_ERR_NODEV;
    }
    r = ops->init(d);
    if (r == HAL_OK) {
        d->flags |= HAL_DEV_F_READY;
        cache_device(d);
    }
    return r;
}
