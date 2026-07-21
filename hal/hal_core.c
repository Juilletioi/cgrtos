/**
 * @file hal_core.c
 * @brief HAL 设备注册表、锁与用户 API 分发实现
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

static void cache_device(hal_device_t *dev)
{
    if (!dev || (unsigned)dev->class >= (unsigned)HAL_DEV_CLASS_MAX) {
        return;
    }
    if (!g_by_class[dev->class]) {
        g_by_class[dev->class] = dev;
    }
}

static hal_device_t *dev_console(void)
{
    return g_by_class[HAL_DEV_CONSOLE]
               ? g_by_class[HAL_DEV_CONSOLE]
               : hal_device_find(HAL_DEV_CONSOLE);
}

static hal_device_t *dev_timer(void)
{
    return g_by_class[HAL_DEV_TIMER]
               ? g_by_class[HAL_DEV_TIMER]
               : hal_device_find(HAL_DEV_TIMER);
}

static hal_device_t *dev_irqc(void)
{
    return g_by_class[HAL_DEV_IRQC]
               ? g_by_class[HAL_DEV_IRQC]
               : hal_device_find(HAL_DEV_IRQC);
}

static hal_device_t *dev_ipi(void)
{
    return g_by_class[HAL_DEV_IPI]
               ? g_by_class[HAL_DEV_IPI]
               : hal_device_find(HAL_DEV_IPI);
}

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
 *
 * @param dev 静态寿命的设备描述符；name/ops/class 必填
 * @return HAL_OK；参数错 HAL_ERR_PARAM；满表或已冻结 HAL_ERR_STATE
 *
 * @details 步骤：
 * 1. 校验 dev / name / ops / class 范围。
 * 2. 关本核中断并取 g_reg_lock。
 * 3. 若已冻结 → 解锁恢复中断，返回 HAL_ERR_STATE。
 * 4. 同名已存在 → 视为幂等成功。
 * 5. 表满 → HAL_ERR_STATE。
 * 6. 写入 g_devs[]，置 HAL_DEV_F_REG，cache_device。
 * 7. 释放锁并恢复中断。
 *
 * @note 驱动文件不得调用本函数；由 hal_board_init 统一调用。
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

int hal_device_count(void)
{
    return g_dev_count;
}

hal_device_t *hal_device_get(int index)
{
    if (index < 0 || index >= g_dev_count) {
        return 0;
    }
    return g_devs[index];
}

int hal_registry_frozen(void)
{
    return g_registry_frozen != 0;
}

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
            drv_plic_device(),
            drv_clint_device(),
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
 *
 * @details 步骤：
 * 1. 读 mhartid，越界则按 0。
 * 2. cgrtos_irq_save 关本核中断，保存到 g_console_irq_saved[cpu]。
 * 3. 自旋获取 g_console_lock（非 g_klock）。
 * 4. 标记 nest=1（诊断用）。
 *
 * @warning 持锁期间禁止 yield / 再取 g_klock / 长时间阻塞。
 */
void hal_console_lock(void)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    if (cpu >= CONFIG_MAX_CORES) {
        cpu = 0;
    }
    g_console_irq_saved[cpu] = cgrtos_irq_save();
    cgrtos_spin_lock(&g_console_lock);
    g_console_lock_nest[cpu] = 1;
}

/**
 * @brief 释放控制台输出权
 *
 * @details 步骤：
 * 1. 读 mhartid（同 lock 规则）。
 * 2. 清 nest 标记。
 * 3. 释放 g_console_lock。
 * 4. cgrtos_irq_restore 恢复进入时的 mstatus。
 */
void hal_console_unlock(void)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    if (cpu >= CONFIG_MAX_CORES) {
        cpu = 0;
    }
    g_console_lock_nest[cpu] = 0;
    cgrtos_spin_unlock(&g_console_lock);
    cgrtos_irq_restore(g_console_irq_saved[cpu]);
}

static void irqc_cfg_lock(uint8_t *cpu_out, uint64_t *flags_out)
{
    uint8_t cpu = (uint8_t)read_csr(mhartid);
    if (cpu >= CONFIG_MAX_CORES) {
        cpu = 0;
    }
    *cpu_out = cpu;
    *flags_out = cgrtos_irq_save();
    cgrtos_spin_lock(&g_irqc_cfg_lock);
    g_irqc_irq_saved[cpu] = *flags_out;
}

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
 * @return HAL_OK / HAL_ERR_NODEV
 * @details 步骤：1. 取 CONSOLE 设备；2. ops->init；3. 成功则置 READY 并缓存。
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
 * @param c 字符
 * @details 步骤：1. hal_console_lock；2. putc_unlocked；3. unlock。
 */
void hal_console_putc(char c)
{
    hal_console_lock();
    hal_console_putc_unlocked(c);
    hal_console_unlock();
}

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
 * @return HAL_OK / HAL_ERR_NODEV
 * @details 步骤：1. 取 IRQC 设备；2. ops->init；3. 成功则置 READY 并缓存。
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
 * @return 源 ID；0=无 pending；无设备时 0
 * @details 步骤：1. 取 IRQC 设备；2. 调 ops->claim(dev)；3. 返回结果。
 * @note 应用可用；trap 入口应直调驱动 plic_hw_claim，勿绕回本函数。
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
 * @param hart 目标核号
 * @return HAL_OK / HAL_ERR_PARAM / HAL_ERR_NODEV
 * @details 步骤：1. 取 IPI 设备；2. ops->send(dev, hart)；3. 返回状态。
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
