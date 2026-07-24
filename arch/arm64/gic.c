/**
 * @file gic.c
 * @brief GICv3（QEMU virt）— HAL IRQC 驱动 + IRQ 分发入口
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * ## 分层
 * - ops / drv_irqc_device() → 供 HAL 注册与用户 hal_irqc_* 分发。
 * - arm64_handle_irq → **直接**调本文件 GIC CPU 接口（IAR/EOIR），禁止再进 hal_irqc_*。
 * - Distributor（GICD）全局；Redistributor（GICR）每 CPU；CPU 接口经 ICC_*_EL1 系统寄存器。
 *
 * QEMU virt：Affinity routing + Group1 NS；Virtual timer PPI = HAL_BOARD_TIMER_IRQ。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

/** @brief GICD_CTLR：Distributor 控制寄存器 */
#define GICD_CTLR     (*(volatile uint32_t *)(HAL_BOARD_GICD_BASE + 0x0000))
/** @brief GICD_IGROUPR(n)：SPI 分组寄存器字 n */
#define GICD_IGROUPR(n) (*(volatile uint32_t *)(HAL_BOARD_GICD_BASE + 0x0080 + (n) * 4))
/** @brief GICD_ISENABLER(n)：SPI 使能置位寄存器字 n @warning 写 1 置使能，写 0 无效 */
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(HAL_BOARD_GICD_BASE + 0x0100 + (n) * 4))
/** @brief GICD_IPRIORITYR(n)：SPI 优先级字节 n */
#define GICD_IPRIORITYR(n) (*(volatile uint8_t *)(HAL_BOARD_GICD_BASE + 0x0400 + (n)))
/** @brief GICD_ICFGR(n)：SPI 边沿/电平配置字 n */
#define GICD_ICFGR(n) (*(volatile uint32_t *)(HAL_BOARD_GICD_BASE + 0x0c00 + (n) * 4))

/** @brief 每 CPU Redistributor 步长（128 KiB；SGI_base 在 +64 KiB） */
#define GICR_STRIDE   0x20000ULL
/**
 * @brief 计算指定 CPU 的 GICR 基址
 * @warning 仅作地址算术；cpu 须 < 已映射 Redistributor 数量
 */
#define GICR_BASE(cpu) (HAL_BOARD_GICR_BASE + (uint64_t)(cpu) * GICR_STRIDE)
/** @brief GICR_CTLR(cpu)：本 CPU Redistributor 控制 */
#define GICR_CTLR(cpu)    (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x0000))
/** @brief GICR_WAKER(cpu)：唤醒/休眠控制 @warning 清 ProcessorSleep 后须等 ChildrenAsleep 清零 */
#define GICR_WAKER(cpu)   (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x0014))
/** @brief GICR_IGROUPR0(cpu)：SGI/PPI 分组 */
#define GICR_IGROUPR0(cpu) (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x10000 + 0x0080))
/** @brief GICR_ISENABLER0(cpu)：SGI/PPI 使能置位 @warning 写 1 置使能 */
#define GICR_ISENABLER0(cpu) (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x10000 + 0x0100))
/** @brief GICR_IPRIORITYR(cpu, n)：SGI/PPI 优先级字节 n */
#define GICR_IPRIORITYR(cpu, n) (*(volatile uint8_t *)(GICR_BASE(cpu) + 0x10000 + 0x0400 + (n)))
/** @brief GICR_ICFGR1(cpu)：PPI 边沿/电平配置 */
#define GICR_ICFGR1(cpu) (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x10000 + 0x0c04))

/**
 * @brief 使能 ICC_SRE_EL1 系统寄存器访问 GIC CPU 接口
 * @details
 * 1. 读 ICC_SRE_EL1（S3_0_C12_C12_5）。
 * 2. 置 SRE|DFB|DIB（低 3 位）。
 * 3. 写回并 ISB 同步。
 * @return 无
 * @retval 无
 * @note 每 CPU 在 gic_hw_init 中调用一次
 * @warning 须在 EL1；写后依赖 ISB 保证后续 ICC_* 可见
 * @attention ❌ ISR-safe（仅初始化路径）；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline void gic_write_sre(void)
{
    uint64_t sre;
    __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre)); /* ICC_SRE_EL1 */
    sre |= 0x7; /* SRE | DFB | DIB */
    __asm__ volatile("msr S3_0_C12_C12_5, %0" :: "r"(sre));
    __asm__ volatile("isb");
}

/**
 * @brief 写 ICC_PMR_EL1 优先级屏蔽寄存器
 * @details
 * 1. 将 v 写入 ICC_PMR_EL1（S3_0_C4_C6_0）。
 * 2. 优先级数值 >= PMR 的中断被屏蔽（GICv3 约定）。
 * @param[in] v 新 PMR 值（通常 0..0xff）
 * @return 无
 * @retval 无
 * @note 与 PLIC threshold 语义对齐，供 set_threshold 使用
 * @warning 过低 PMR 会屏蔽几乎全部 IRQ
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline void gic_write_pmr(uint32_t v)
{
    __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"((uint64_t)v)); /* ICC_PMR_EL1 */
}

/**
 * @brief 写 ICC_BPR1_EL1 Group1 二进制点寄存器
 * @details
 * 1. 将 v 写入 ICC_BPR1_EL1（S3_0_C12_C12_3）。
 * 2. 影响优先级分组粒度。
 * @param[in] v BPR 值；本驱动 init 写 0
 * @return 无
 * @retval 无
 * @note 仅初始化路径使用
 * @warning 运行期随意改写会改变抢占行为
 * @attention ❌ ISR-safe（仅初始化）；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline void gic_write_bpr1(uint32_t v)
{
    __asm__ volatile("msr S3_0_C12_C12_3, %0" :: "r"((uint64_t)v)); /* ICC_BPR1_EL1 */
}

/**
 * @brief 写 ICC_IGRPEN1_EL1 使能 Group1 中断向 CPU 投递
 * @details
 * 1. 将 v 写入 ICC_IGRPEN1_EL1（S3_0_C12_C12_7）。
 * 2. v=1 打开 Group1（NS）投递。
 * @param[in] v 0 关闭 / 非 0 使能
 * @return 无
 * @retval 无
 * @note 须在 SRE 已使能之后调用
 * @warning 写 0 会停止本 CPU 全部 Group1 IRQ
 * @attention ❌ ISR-safe（仅初始化）；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline void gic_write_igrpen1(uint32_t v)
{
    __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"((uint64_t)v)); /* ICC_IGRPEN1_EL1 */
}

/**
 * @brief 读 ICC_IAR1_EL1 应答最高优先级 pending Group1 中断
 * @details
 * 1. MRS 读 ICC_IAR1_EL1（S3_0_C12_C12_0）。
 * 2. 返回 INTID；1020..1023 为特殊值（无 pending / spurious）。
 * @return 已应答的 INTID
 * @retval 0..1019 有效中断号
 * @retval 1020..1023 无有效 pending（spurious）
 * @note 读即 ack；须配对 EOIR
 * @warning 未 EOI 前同源不可再次投递
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline uint32_t gic_read_iar1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v)); /* ICC_IAR1_EL1 */
    return (uint32_t)v;
}

/**
 * @brief 写 ICC_EOIR1_EL1 结束 Group1 中断处理
 * @details
 * 1. 将 irq（先前 IAR 返回值）写入 ICC_EOIR1_EL1（S3_0_C12_C12_1）。
 * 2. 降低运行优先级，允许同级/更低优先级再次投递。
 * @param[in] irq 先前 gic_read_iar1 返回的 INTID
 * @return 无
 * @retval 无
 * @note 须与 IAR 严格配对
 * @warning 写入错误 INTID 会导致优先级状态错乱
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static inline void gic_write_eoir1(uint32_t irq)
{
    __asm__ volatile("msr S3_0_C12_C12_1, %0" :: "r"((uint64_t)irq)); /* ICC_EOIR1_EL1 */
}

/**
 * @brief 唤醒本 CPU 的 GIC Redistributor
 * @details
 * 1. 清 GICR_WAKER.ProcessorSleep（bit1）。
 * 2. 轮询直至 ChildrenAsleep（bit2）清零。
 * @param[in] cpu 逻辑 CPU 编号（与 Redistributor 索引一致）
 * @return 无
 * @retval 无
 * @note 每 CPU 在 gic_hw_init 中调用
 * @warning 硬件异常时可能忙等；正常 virt 会很快退出
 * @attention ❌ ISR-safe（仅初始化；可能忙等）；✅ 可能阻塞（轮询 WAKER）
 * @internal
 */
static void gic_rdist_wake(unsigned cpu)
{
    GICR_WAKER(cpu) &= ~(1u << 1); /* clear ProcessorSleep */
    while (GICR_WAKER(cpu) & (1u << 2)) {
        /* ChildrenAsleep */
    }
}

/**
 * @brief 配置并使能本 CPU 的 PPI/SGI（INTID < 32）
 * @details
 * 1. 写 GICR_IPRIORITYR(cpu, intid) = prio。
 * 2. GICR_IGROUPR0 对应位置 1（Group1）。
 * 3. GICR_ISENABLER0 写 1 使能该 INTID。
 * @param[in] cpu   逻辑 CPU 编号
 * @param[in] intid PPI/SGI 号（0..31）
 * @param[in] prio  8 位优先级
 * @return 无
 * @retval 无
 * @note 用于 Virtual timer 等 PPI
 * @warning intid 须 < 32；越界写 Redistributor 未定义
 * @attention ❌ ISR-safe（初始化/配置路径）；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static void gic_enable_ppi(unsigned cpu, uint32_t intid, uint8_t prio)
{
    GICR_IPRIORITYR(cpu, intid) = prio;
    GICR_IGROUPR0(cpu) |= (1u << intid);
    GICR_ISENABLER0(cpu) = (1u << intid);
}

/**
 * @brief 配置并使能全局 SPI（INTID >= 32）
 * @details
 * 1. 写 GICD_IPRIORITYR(intid) = prio。
 * 2. GICD_IGROUPR 对应位置 1（Group1）。
 * 3. GICD_ISENABLER 写 1 使能该 INTID。
 * @param[in] intid SPI 号（通常 32..1019）
 * @param[in] prio  8 位优先级
 * @return 无
 * @retval 无
 * @note 本驱动在 CPU0 init 时可选使能 UART SPI
 * @warning 多核并发改 GICD 须外层串行化
 * @attention ❌ ISR-safe（配置路径）；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static void gic_enable_spi(uint32_t intid, uint8_t prio)
{
    unsigned n = intid / 32u;
    GICD_IPRIORITYR(intid) = prio;
    GICD_IGROUPR(n) |= (1u << (intid % 32u));
    GICD_ISENABLER(n) = (1u << (intid % 32u));
}

/**
 * @brief GIC 本 CPU 硬件初始化
 * @details
 * 1. 全局首次：cgrtos_irq_init()；GICD_CTLR 开 Affinity routing + EnableGrp1NS，等 RWP 清。
 * 2. gic_rdist_wake 唤醒本 CPU Redistributor。
 * 3. SRE / PMR=0xff / BPR1=0 / IGRPEN1=1。
 * 4. 使能 Virtual timer PPI；CPU0 额外使能 UART SPI。
 * 5. daifclr #2 解除 IRQ 屏蔽。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return HAL_OK
 * @retval HAL_OK 成功
 * @note 由 HAL hal_irqc_init 经 ops->init 调用
 * @warning 驱动层禁止调用 hal_* 用户 API；rdist_wake 可能忙等
 * @attention ❌ ISR-safe；✅ 可能阻塞（轮询 GICD RWP / GICR WAKER）
 * @internal
 */
static hal_status_t gic_hw_init(hal_device_t *dev)
{
    (void)dev;
    static uint8_t irq_inited;
    unsigned cpu = arch_cpu_id();

    if (!irq_inited) {
        cgrtos_irq_init();
        /* Distributor: Affinity routing + EnableGrp1NS */
        GICD_CTLR = (1u << 4) | (1u << 1);
        while (GICD_CTLR & (1u << 31)) {
        }
        irq_inited = 1;
    }

    gic_rdist_wake(cpu);
    gic_write_sre();
    gic_write_pmr(0xff);
    gic_write_bpr1(0);
    gic_write_igrpen1(1);

    /* Virtual timer PPI 27 */
    gic_enable_ppi(cpu, HAL_BOARD_TIMER_IRQ, 0xa0);
    /* Optional UART SPI */
    if (cpu == 0) {
        gic_enable_spi(HAL_BOARD_UART_IRQ, 0xa0);
    }

    __asm__ volatile("msr daifclr, #2"); /* unmask IRQ */
    return HAL_OK;
}

/**
 * @brief 读 ICC_IAR1 取最高优先级 pending INTID（HAL claim）
 * @details
 * 1. 忽略 dev。
 * 2. 返回 gic_read_iar1()。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return 已 claim 的 INTID
 * @retval 0..1019 有效
 * @retval 1020..1023 spurious / 无 pending
 * @note 每 CPU 独立 CPU 接口
 * @warning claim 后须 complete（EOIR），否则同源无法再次触发
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static uint32_t gic_hw_claim(hal_device_t *dev)
{
    (void)dev;
    return gic_read_iar1();
}

/**
 * @brief 写 ICC_EOIR1 完成中断（HAL complete）
 * @details
 * 1. 忽略 dev。
 * 2. gic_write_eoir1(irq)。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] irq 先前 claim 返回的 INTID
 * @return 无
 * @retval 无
 * @note 须与 claim 配对；trap 路径亦可直调 gic_write_eoir1
 * @warning irq 须为最近一次 IAR 返回值
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static void gic_hw_complete(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    gic_write_eoir1(irq);
}

/**
 * @brief 经 ICC_PMR_EL1 设置优先级屏蔽（对齐 PLIC threshold）
 * @details
 * 1. 将 thr 钳位到 0..0xff。
 * 2. gic_write_pmr。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] thr 新 PMR；>0xff 钳为 0xff
 * @return 无
 * @retval 无
 * @note 数值越大允许的优先级范围越宽（本驱动 init 用 0xff）
 * @warning 错误 PMR 会导致 IRQ 饥饿或风暴
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static void gic_hw_set_threshold(hal_device_t *dev, uint32_t thr)
{
    (void)dev;
    gic_write_pmr(thr > 0xffu ? 0xffu : thr);
}

/**
 * @brief 读当前 ICC_PMR_EL1
 * @details
 * 1. MRS 读 ICC_PMR_EL1。
 * 2. 返回低 32 位。
 * @param[in] dev 设备描述符；本驱动未使用
 * @return 当前 PMR 值
 * @retval 0..0xff 典型范围
 * @note 只读系统寄存器
 * @warning 无
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static uint32_t gic_hw_get_threshold(hal_device_t *dev)
{
    uint64_t v;
    (void)dev;
    __asm__ volatile("mrs %0, S3_0_C4_C6_0" : "=r"(v));
    return (uint32_t)v;
}

/**
 * @brief 设置 GIC 中断源优先级
 * @details
 * 1. 校验 irq 非 0、<1020，且 prio <= CONFIG_IRQ_PRIORITY_MAX。
 * 2. irq<32 写本 CPU GICR_IPRIORITYR；否则写 GICD_IPRIORITYR。
 * @param[in] dev  设备描述符；本驱动未使用
 * @param[in] irq  INTID
 * @param[in] prio 优先级
 * @return HAL_OK 或 HAL_ERR_PARAM
 * @retval HAL_OK        成功
 * @retval HAL_ERR_PARAM 参数非法
 * @note 与 PLIC 契约对齐：irq 0 保留
 * @warning 运行期修改建议经 HAL 配置锁保护
 * @attention ❌ ISR-safe（配置路径）；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t gic_hw_set_priority(hal_device_t *dev, uint32_t irq, uint32_t prio)
{
    (void)dev;
    /* 与 PLIC 契约对齐：irq 0 保留；优先级超上限拒绝 */
    if (irq == 0 || irq >= 1020u || prio > CONFIG_IRQ_PRIORITY_MAX) {
        return HAL_ERR_PARAM;
    }
    if (irq < 32u) {
        GICR_IPRIORITYR(arch_cpu_id(), irq) = (uint8_t)prio;
    } else {
        GICD_IPRIORITYR(irq) = (uint8_t)prio;
    }
    return HAL_OK;
}

/**
 * @brief 读 GIC 中断源优先级
 * @details
 * 1. 非法 irq（0 或 ->=1020）返回 0。
 * 2. irq<32 读本 CPU GICR；否则读 GICD。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] irq INTID
 * @return 源优先级字节
 * @retval 0..255 有效或非法时的 0
 * @note 只读 MMIO
 * @warning 无
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static uint32_t gic_hw_get_priority(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    if (irq == 0 || irq >= 1020u) {
        return 0;
    }
    if (irq < 32u) {
        return GICR_IPRIORITYR(arch_cpu_id(), irq);
    }
    return GICD_IPRIORITYR(irq);
}

/**
 * @brief 使能指定 INTID
 * @details
 * 1. 校验 irq 非 0、<1020。
 * 2. irq<32 写 GICR_ISENABLER0；否则写 GICD_ISENABLER。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] irq INTID
 * @return HAL_OK 或 HAL_ERR_PARAM
 * @retval HAL_OK        成功
 * @retval HAL_ERR_PARAM 参数非法
 * @note ISENABLER 为置位寄存器（写 1 有效）
 * @warning 无
 * @attention ❌ ISR-safe（配置路径）；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t gic_hw_enable(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    if (irq == 0 || irq >= 1020u) {
        return HAL_ERR_PARAM;
    }
    if (irq < 32u) {
        GICR_ISENABLER0(arch_cpu_id()) = (1u << irq);
    } else {
        GICD_ISENABLER(irq / 32u) = (1u << (irq % 32u));
    }
    return HAL_OK;
}

/**
 * @brief 禁用指定 INTID
 * @details
 * 1. 校验 irq 非 0、<1020。
 * 2. 写对应 ICENABLER（GICR +0x0180 或 GICD +0x0180）置位清除使能。
 * @param[in] dev 设备描述符；本驱动未使用
 * @param[in] irq INTID
 * @return HAL_OK 或 HAL_ERR_PARAM
 * @retval HAL_OK        成功
 * @retval HAL_ERR_PARAM 参数非法
 * @note ICENABLER 写 1 清除使能
 * @warning 禁用正在处理的源须配合软件协议
 * @attention ❌ ISR-safe（配置路径）；❌ 不阻塞、不引起上下文切换
 * @internal
 */
static hal_status_t gic_hw_disable(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    if (irq == 0 || irq >= 1020u) {
        return HAL_ERR_PARAM;
    }
    if (irq < 32u) {
        *(volatile uint32_t *)(GICR_BASE(arch_cpu_id()) + 0x10000 + 0x0180) = (1u << irq);
    } else {
        *(volatile uint32_t *)(HAL_BOARD_GICD_BASE + 0x0180 + (irq / 32u) * 4) =
            (1u << (irq % 32u));
    }
    return HAL_OK;
}

static const hal_irqc_ops_t s_gic_ops = {
    .init = gic_hw_init,
    .claim = gic_hw_claim,
    .complete = gic_hw_complete,
    .set_threshold = gic_hw_set_threshold,
    .get_threshold = gic_hw_get_threshold,
    .set_priority = gic_hw_set_priority,
    .get_priority = gic_hw_get_priority,
    .enable = gic_hw_enable,
    .disable = gic_hw_disable,
};

static hal_device_t s_gic_dev = {
    .name = "gic0",
    .class = HAL_DEV_IRQC,
    .mmio_base = HAL_BOARD_GICD_BASE,
    .ops = &s_gic_ops,
    .priv = 0,
    .flags = 0,
};

/**
 * @brief 向 HAL 导出 GIC IRQC 设备描述符
 * @details
 * 1. 返回静态 s_gic_dev 指针。
 * 2. 注册由 hal_board_init → hal_device_register 完成。
 * @return 非 NULL 静态设备指针；生命周期 = 系统寿命
 * @retval 非 NULL 成功
 * @note 勿释放返回值；应用应使用 hal_irqc_*
 * @warning 驱动不得自行调用 hal_device_register
 * @attention ✅ ISR-safe；❌ 不阻塞、不引起上下文切换
 */
hal_device_t *drv_irqc_device(void)
{
    return &s_gic_dev;
}

extern void arm64_handle_timer(void);

/**
 * @brief AArch64 IRQ 入口：IAR → timer / dispatch → EOIR
 * @details
 * 1. cgrtos_isr_enter 标记 ISR 上下文。
 * 2. gic_read_iar1 取 INTID；<1020 则处理：
 *    - timer PPI → arm64_handle_timer；
 *    - 其它 → cgrtos_irq_dispatch 并置 g_yield_pending。
 * 3. gic_write_eoir1 结束中断。
 * 4. cgrtos_isr_exit。
 * @param[in] f 异常栈帧指针；本实现未使用
 * @return 无
 * @retval 无
 * @note 由向量表直接调用；禁止再绕回 hal_irqc_*
 * @warning 须在 IRQ 上下文；timer 路径可能置 yield
 * @attention ✅ ISR-safe；✅ 可能引起上下文切换（置 g_yield_pending / tick）
 */
void arm64_handle_irq(uint64_t *f)
{
    uint32_t irq;
    (void)f;
    cgrtos_isr_enter();
    irq = gic_read_iar1();
    if (irq < 1020u) {
        if (irq == HAL_BOARD_TIMER_IRQ) {
            arm64_handle_timer();
        } else {
            cgrtos_irq_dispatch(irq);
            g_yield_pending[arch_cpu_id()] = 1;
        }
        gic_write_eoir1(irq);
    }
    cgrtos_isr_exit();
}
