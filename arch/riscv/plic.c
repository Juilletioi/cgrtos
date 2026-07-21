/**
 * @file plic.c
 * @brief RISC-V PLIC 板级驱动（纯硬件层）+ 外部中断 trap 入口
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
#include "../../hal/hal_board.h"

/**
 * @brief PLIC 本 hart 初始化
 *
 * @details 步骤：
 * 1. 全局首次调用 cgrtos_irq_init() 清 handler 表。
 * 2. 本 hart threshold 写 0（放行 priority>0）。
 * 3. 打开 mie.MEIE。
 * 4. 返回 HAL_OK。
 */
static hal_status_t plic_hw_init(hal_device_t *dev)
{
    (void)dev;
    static uint8_t irq_inited;
    if (!irq_inited) {
        cgrtos_irq_init();
        irq_inited = 1;
    }
    uint64_t h = read_csr(mhartid);
    *(volatile uint32_t *)HAL_BOARD_PLIC_THRESHOLD(h) = 0;
    set_csr_bits(mie, 0x800);
    return HAL_OK;
}

/** @brief claim：读本 hart claim 寄存器 */
static uint32_t plic_hw_claim(hal_device_t *dev)
{
    (void)dev;
    uint64_t h = read_csr(mhartid);
    return *(volatile uint32_t *)HAL_BOARD_PLIC_CLAIM(h);
}

/** @brief complete：写本 hart claim 寄存器 */
static void plic_hw_complete(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    uint64_t h = read_csr(mhartid);
    *(volatile uint32_t *)HAL_BOARD_PLIC_CLAIM(h) = irq;
}

/** @brief 写本 hart threshold */
static void plic_hw_set_threshold(hal_device_t *dev, uint32_t thr)
{
    (void)dev;
    uint64_t h = read_csr(mhartid);
    *(volatile uint32_t *)HAL_BOARD_PLIC_THRESHOLD(h) = thr;
}

/** @brief 读本 hart threshold */
static uint32_t plic_hw_get_threshold(hal_device_t *dev)
{
    (void)dev;
    uint64_t h = read_csr(mhartid);
    return *(volatile uint32_t *)HAL_BOARD_PLIC_THRESHOLD(h);
}

/**
 * @brief 设置源优先级
 * @details 步骤：1. 校验 irq/prio；2. 写 PRIORITY(irq)；3. 返回状态。
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

static uint32_t plic_hw_get_priority(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return 0;
    }
    return *(volatile uint32_t *)HAL_BOARD_PLIC_PRIORITY(irq);
}

/**
 * @brief 对本 hart 使能源
 * @details 步骤：1. 校验；2. word=irq/32 bit=irq%32；3. enable 字按位置 1。
 */
static hal_status_t plic_hw_enable(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return HAL_ERR_PARAM;
    }
    uint64_t h = read_csr(mhartid);
    uint32_t word = irq / 32U;
    uint32_t bit = irq % 32U;
    volatile uint32_t *en =
        (volatile uint32_t *)HAL_BOARD_PLIC_ENABLE(h, word);
    *en |= (1U << bit);
    return HAL_OK;
}

/** @brief 对本 hart 禁用源（步骤同 enable，改为按位清 0） */
static hal_status_t plic_hw_disable(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    if (irq == 0 || irq > CONFIG_IRQ_MAX_SOURCES) {
        return HAL_ERR_PARAM;
    }
    uint64_t h = read_csr(mhartid);
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

hal_device_t *drv_plic_device(void)
{
    return &s_plic_dev;
}

/**
 * @brief 机器外部中断 C 入口（底层直调驱动）
 *
 * @details 步骤：
 * 1. cgrtos_isr_enter 标记 ISR 上下文。
 * 2. plic_hw_claim 取最高优先级 pending 源；0 则跳到步骤 6。
 * 3. 保存 threshold；抬到本源 priority（0 则抬到 1），屏蔽同级及以下。
 * 4. 可选嵌套：清 MTIE|MSIE，开 mstatus.MIE，仅允许更高优先级外部中断。
 * 5. cgrtos_irq_dispatch → 恢复 mie/threshold → plic_hw_complete。
 * 6. cgrtos_isr_exit。
 *
 * @note 本路径故意不调用 hal_irqc_*，避免「底层 → HAL」倒置。
 */
void riscv_handle_external(uint64_t *f)
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
 * @param v 数值
 * @details 步骤：1. 输出 "0x"；2. 从高半字节到低半字节逐位输出。
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
 * @brief 同步异常诊断（底层直写 UART，不调用 HAL / printf）
 *
 * @details 步骤：
 * 1. 忽略栈帧指针。
 * 2. drv_uart_early_puts 输出前缀。
 * 3. 以十六进制粗打 cause / epc（避免依赖格式化库与 HAL）。
 */
void riscv_handle_exception(uint64_t *f, uint64_t cause, uint64_t epc)
{
    (void)f;
    drv_uart_early_puts("[EXC] cause=");
    early_put_hex((unsigned long)cause);
    drv_uart_early_puts(" epc=");
    early_put_hex((unsigned long)epc);
    drv_uart_early_putc('\n');
}
