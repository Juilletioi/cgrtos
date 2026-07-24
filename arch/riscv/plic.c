/**
 * @file plic.c
 * @brief RISC-V PLIC 板级驱动（纯硬件层）+ 外部中断 trap 入口
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * ## 分层
 * - ops / drv_plic_device() → 供 HAL 注册与用户 hal_irqc_* 分发。
 * - riscv_handle_external → **直接**调本文件 plic_hw_*，禁止再进 hal_irqc_*。
 * - 兼容 cgrtos_plic_* 在 hal/hal_compat.c，不在本文件。
 *
 * Nuclei evalsoc：M-mode context = hart * 2。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

/**
 * @brief PLIC 本 hart 硬件初始化
 * @details
 * 1. 全局首次调用 cgrtos_irq_init() 清 handler 表。
 * 2. 本 hart threshold 写 0（放行 priority>0）。
 * 3. 打开 mie.MEIE 使能外部中断。
 * 4. 返回 HAL_OK。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_irqc_init 经 ops->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t plic_hw_init(hal_device_t *dev)
{
    (void)dev;
    static uint8_t irq_inited;
    if (!irq_inited) {
        cgrtos_irq_init();
        irq_inited = 1;
    }
    uint64_t h = arch_cpu_id();
    *(volatile uint32_t *)HAL_BOARD_PLIC_THRESHOLD(h) = 0;
    set_csr_bits(mie, 0x800);
    return HAL_OK;
}

/**
 * @brief 读本 hart PLIC claim 寄存器，取最高优先级 pending 源
 * @details
 * 1. 读 mhartid 确定本 hart。
 * 2. 读 HAL_BOARD_PLIC_CLAIM(h) 寄存器。
 * 3. 返回源 ID；0 表示无 pending。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return 已 claim 的中断源号
 * @retval >0 有效源 ID
 * @retval 0   无 pending 中断
 * @note 每 hart 独立 claim 上下文
 * @warning claim 后须 complete，否则同源无法再次触发
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static uint32_t plic_hw_claim(hal_device_t *dev)
{
    (void)dev;
    uint64_t h = arch_cpu_id();
    return *(volatile uint32_t *)HAL_BOARD_PLIC_CLAIM(h);
}

/**
 * @brief 写本 hart PLIC complete 寄存器，完成中断处理
 * @details
 * 1. 读 mhartid 确定本 hart。
 * 2. 向 HAL_BOARD_PLIC_CLAIM(h) 写入 irq 完成本次中断。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] irq 先前 claim 返回的源号
 * @return 无
 * @retval 无
 * @note 须与 claim 配对；trap 路径直调，不经 hal_irqc_complete
 * @warning irq 须为最近一次 claim 的源号
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static void plic_hw_complete(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    uint64_t h = arch_cpu_id();
    *(volatile uint32_t *)HAL_BOARD_PLIC_CLAIM(h) = irq;
}

/**
 * @brief 写本 hart PLIC 优先级阈值
 * @details
 * 1. 读 mhartid 确定本 hart。
 * 2. 写 HAL_BOARD_PLIC_THRESHOLD(h) = thr。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] thr 新阈值；0 允许全部 priority>0
 * @return 无
 * @retval 无
 * @note 仅屏蔽 priority <= threshold 的源
 * @warning threshold 过高会导致低优先级源长期得不到服务
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static void plic_hw_set_threshold(hal_device_t *dev, uint32_t thr)
{
    (void)dev;
    uint64_t h = arch_cpu_id();
    *(volatile uint32_t *)HAL_BOARD_PLIC_THRESHOLD(h) = thr;
}

/**
 * @brief 读本 hart PLIC 优先级阈值
 * @details
 * 1. 读 mhartid 确定本 hart。
 * 2. 读 HAL_BOARD_PLIC_THRESHOLD(h) 并返回。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return 当前 threshold 值
 * @retval >=0 当前阈值
 * @note 每 hart 独立 threshold
 * @warning 无
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static uint32_t plic_hw_get_threshold(hal_device_t *dev)
{
    (void)dev;
    uint64_t h = arch_cpu_id();
    return *(volatile uint32_t *)HAL_BOARD_PLIC_THRESHOLD(h);
}

/**
 * @brief 设置 PLIC 中断源优先级
 * @details
 * 1. 校验 irq 在 1..CONFIG_IRQ_MAX_SOURCES 且 prio <= CONFIG_IRQ_PRIORITY_MAX。
 * 2. 写 HAL_BOARD_PLIC_PRIORITY(irq) = prio。
 * 3. 返回 HAL_OK 或 HAL_ERR_PARAM。
 * @param[in] dev  设备描述符；本驱动未使用
 * @param[in] irq  中断源号
 * @param[in] prio 优先级 0..CONFIG_IRQ_PRIORITY_MAX
 * @return HAL_OK 或 HAL_ERR_PARAM
 * @retval HAL_OK        成功
 * @retval HAL_ERR_PARAM 参数非法
 * @note priority 0 表示永不触发；全局共享寄存器
 * @warning 运行期修改建议经 HAL 配置锁保护
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t plic_hw_set_priority(hal_device_t *dev, uint32_t irq, uint32_t prio)
{
    (void)dev;
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return HAL_ERR_PARAM;
    }
    if (prio > CONFIG_IRQ_PRIORITY_MAX) {
        return HAL_ERR_PARAM;
    }
    *(volatile uint32_t *)HAL_BOARD_PLIC_PRIORITY(irq) = prio;
    return HAL_OK;
}

/**
 * @brief 读 PLIC 中断源优先级
 * @details
 * 1. 校验 irq 在 1..CONFIG_IRQ_MAX_SOURCES。
 * 2. 读 HAL_BOARD_PLIC_PRIORITY(irq) 并返回；非法 irq 返回 0。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] irq 中断源号
 * @return 源优先级
 * @retval >0 有效优先级
 * @retval 0   非法 irq 或未配置
 * @note 只读 MMIO
 * @warning 无
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static uint32_t plic_hw_get_priority(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return 0;
    }
    return *(volatile uint32_t *)HAL_BOARD_PLIC_PRIORITY(irq);
}

/**
 * @brief 对本 hart 使能 PLIC 中断源
 * @details
 * 1. 校验 irq 在 1..CONFIG_IRQ_MAX_SOURCES。
 * 2. word = irq/32，bit = irq%32。
 * 3. 对应 enable 字按位置 1。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] irq 中断源号
 * @return HAL_OK 或 HAL_ERR_PARAM
 * @retval HAL_OK        成功
 * @retval HAL_ERR_PARAM 参数非法
 * @note 每 hart 独立 enable 位图
 * @warning RMW 操作；并发修改须加锁
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t plic_hw_enable(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return HAL_ERR_PARAM;
    }
    uint64_t h = arch_cpu_id();
    uint32_t word = irq / 32U;
    uint32_t bit = irq % 32U;
    volatile uint32_t *en =
        (volatile uint32_t *)HAL_BOARD_PLIC_ENABLE(h, word);
    *en |= (1U << bit);
    return HAL_OK;
}

/**
 * @brief 对本 hart 禁用 PLIC 中断源
 * @details
 * 1. 校验 irq 在 1..CONFIG_IRQ_MAX_SOURCES。
 * 2. word = irq/32，bit = irq%32。
 * 3. 对应 enable 字按位清 0。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] irq 中断源号
 * @return HAL_OK 或 HAL_ERR_PARAM
 * @retval HAL_OK        成功
 * @retval HAL_ERR_PARAM 参数非法
 * @note 步骤同 enable，改为按位清 0
 * @warning RMW 操作；并发修改须加锁
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t plic_hw_disable(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return HAL_ERR_PARAM;
    }
    uint64_t h = arch_cpu_id();
    uint32_t word = irq / 32U;
    uint32_t bit = irq % 32U;
    volatile uint32_t *en =
        (volatile uint32_t *)HAL_BOARD_PLIC_ENABLE(h, word);
    *en &= ~(1U << bit);
    return HAL_OK;
}

static const hal_irqc_ops_t s_plic_ops = {
    .init          = plic_hw_init,
    .claim         = plic_hw_claim,
    .complete      = plic_hw_complete,
    .set_threshold = plic_hw_set_threshold,
    .get_threshold = plic_hw_get_threshold,
    .set_priority  = plic_hw_set_priority,
    .get_priority  = plic_hw_get_priority,
    .enable        = plic_hw_enable,
    .disable       = plic_hw_disable,
};

static hal_device_t s_plic_dev = {
    .name      = "plic0",
    .class     = HAL_DEV_IRQC,
    .mmio_base = HAL_BOARD_PLIC_BASE,
    .ops       = &s_plic_ops,
    .priv      = 0,
    .flags     = 0,
};

/**
 * @brief 向 HAL 导出 PLIC 设备描述符
 * @details
 * 1. 返回静态 s_plic_dev 指针。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 勿释放返回值；应用应使用 hal_irqc_*
 * @warning 驱动不得自行调用 hal_device_register
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 */
hal_device_t *drv_irqc_device(void)
{
    return &s_plic_dev;
}

/**
 * @brief 外部中断 C 入口（底层直调驱动，不经 HAL）
 * @details
 * 1. cgrtos_isr_enter 标记 ISR 上下文。
 * 2. plic_hw_claim 取最高优先级 pending 源；0 则跳到步骤 6。
 * 3. 保存 threshold；抬到本源 priority（0 则抬到 1），屏蔽同级及以下。
 * 4. 可选嵌套：清 MTIE|MSIE，开 mstatus.MIE，仅允许更高优先级外部中断。
 * 5. cgrtos_irq_dispatch → 恢复 mie/threshold → plic_hw_complete。
 * 6. cgrtos_isr_exit 退出 ISR 上下文。
 * @param[in] f 陷阱栈帧指针；本实现未使用
 * @return 无
 * @retval 无
 * @note 本路径故意不调用 hal_irqc_*，避免「底层 → HAL」倒置
 * @warning 须在中断上下文调用；不可从任务直接调用
 * @attention ✅ ISR-safe；✅ 可能引起上下文切换（irq dispatch）
 */
void arch_handle_external(uint64_t *f)
{
    (void)f;
    cgrtos_isr_enter();

    uint32_t irq = plic_hw_claim(&s_plic_dev);
    if (irq) {
        uint32_t saved_thr = plic_hw_get_threshold(&s_plic_dev);
        uint32_t prio = plic_hw_get_priority(&s_plic_dev, irq);
        if (prio == 0) {
            prio = 1;
        }
        plic_hw_set_threshold(&s_plic_dev, prio);

#if CONFIG_IRQ_NESTING
        uint64_t mie_save;
        asm volatile("csrr %0, mie" : "=r"(mie_save));
        asm volatile("csrc mie, %0" :: "r"(0x88ULL));
        set_csr_bits(mstatus, 0x8);
#endif
        cgrtos_irq_dispatch(irq);
#if CONFIG_IRQ_NESTING
        clear_csr_bits(mstatus, 0x8);
        asm volatile("csrw mie, %0" :: "r"(mie_save));
#endif

        plic_hw_set_threshold(&s_plic_dev, saved_thr);
        plic_hw_complete(&s_plic_dev, irq);
    }

    cgrtos_isr_exit();
}

/**
 * @brief 将无符号数以十六进制粗打到早期 UART
 * @details
 * 1. 输出前缀 "0x"。
 * 2. 从高半字节到低半字节逐位输出十六进制字符。
 * @param[in] v 待输出的数值
 * @return 无
 * @retval 无
 * @note 仅供 riscv_handle_exception 诊断；避免依赖 printf
 * @warning 无锁、轮询阻塞
 * @attention ✅ ISR-safe；✅ 阻塞（轮询 TX FIFO）
 * @internal
 */
static void early_put_hex(unsigned long v)
{
    const char *hex = "0123456789abcdef";
    int i;
    drv_uart_early_puts("0x");
    for (i = (int)(sizeof(unsigned long) * 2 - 1); i >= 0; i--) {
        drv_uart_early_putc(hex[(v >> (i * 4)) & 0xFUL]);
    }
}

/**
 * @brief 同步异常诊断输出（底层直写 UART，不调用 HAL / printf）
 * @details
 * 1. 忽略栈帧指针 f。
 * 2. drv_uart_early_puts 输出 "[EXC] cause=" 前缀。
 * 3. 以十六进制粗打 cause / epc 并换行。
 * @param[in] f     陷阱栈帧指针；本实现未使用
 * @param[in] cause mcause 异常码
 * @param[in] epc   mepc 异常 PC
 * @return 无
 * @retval 无
 * @note 由 trap 向量在同步异常路径调用
 * @warning 输出后通常进入挂起；勿依赖完整格式化库
 * @attention ✅ ISR-safe；✅ 阻塞（轮询 TX FIFO）
 */
void arch_handle_exception(uint64_t *f, uint64_t cause, uint64_t epc)
{
    (void)f;
    drv_uart_early_puts("[EXC] cause=");
    early_put_hex((unsigned long)cause);
    drv_uart_early_puts(" epc=");
    early_put_hex((unsigned long)epc);
    drv_uart_early_putc('\n');
}
