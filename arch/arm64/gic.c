/**
 * @file gic.c
 * @brief GICv3（QEMU virt）— HAL IRQC + IRQ 分发
 */
#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"
#include "hal_board.h"

#define GICD_CTLR     (*(volatile uint32_t *)(HAL_BOARD_GICD_BASE + 0x0000))
#define GICD_IGROUPR(n) (*(volatile uint32_t *)(HAL_BOARD_GICD_BASE + 0x0080 + (n) * 4))
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(HAL_BOARD_GICD_BASE + 0x0100 + (n) * 4))
#define GICD_IPRIORITYR(n) (*(volatile uint8_t *)(HAL_BOARD_GICD_BASE + 0x0400 + (n)))
#define GICD_ICFGR(n) (*(volatile uint32_t *)(HAL_BOARD_GICD_BASE + 0x0c00 + (n) * 4))

/* Redistributor: per-CPU stride 128KiB (SGI_base at +64KiB) */
#define GICR_STRIDE   0x20000ULL
#define GICR_BASE(cpu) (HAL_BOARD_GICR_BASE + (uint64_t)(cpu) * GICR_STRIDE)
#define GICR_CTLR(cpu)    (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x0000))
#define GICR_WAKER(cpu)   (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x0014))
#define GICR_IGROUPR0(cpu) (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x10000 + 0x0080))
#define GICR_ISENABLER0(cpu) (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x10000 + 0x0100))
#define GICR_IPRIORITYR(cpu, n) (*(volatile uint8_t *)(GICR_BASE(cpu) + 0x10000 + 0x0400 + (n)))
#define GICR_ICFGR1(cpu) (*(volatile uint32_t *)(GICR_BASE(cpu) + 0x10000 + 0x0c04))

static inline void gic_write_sre(void)
{
    uint64_t sre;
    __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre)); /* ICC_SRE_EL1 */
    sre |= 0x7; /* SRE | DFB | DIB */
    __asm__ volatile("msr S3_0_C12_C12_5, %0" :: "r"(sre));
    __asm__ volatile("isb");
}

static inline void gic_write_pmr(uint32_t v)
{
    __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"((uint64_t)v)); /* ICC_PMR_EL1 */
}

static inline void gic_write_bpr1(uint32_t v)
{
    __asm__ volatile("msr S3_0_C12_C12_3, %0" :: "r"((uint64_t)v)); /* ICC_BPR1_EL1 */
}

static inline void gic_write_igrpen1(uint32_t v)
{
    __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"((uint64_t)v)); /* ICC_IGRPEN1_EL1 */
}

static inline uint32_t gic_read_iar1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v)); /* ICC_IAR1_EL1 */
    return (uint32_t)v;
}

static inline void gic_write_eoir1(uint32_t irq)
{
    __asm__ volatile("msr S3_0_C12_C12_1, %0" :: "r"((uint64_t)irq)); /* ICC_EOIR1_EL1 */
}

static void gic_rdist_wake(unsigned cpu)
{
    GICR_WAKER(cpu) &= ~(1u << 1); /* clear ProcessorSleep */
    while (GICR_WAKER(cpu) & (1u << 2)) {
        /* ChildrenAsleep */
    }
}

static void gic_enable_ppi(unsigned cpu, uint32_t intid, uint8_t prio)
{
    GICR_IPRIORITYR(cpu, intid) = prio;
    GICR_IGROUPR0(cpu) |= (1u << intid);
    GICR_ISENABLER0(cpu) = (1u << intid);
}

static void gic_enable_spi(uint32_t intid, uint8_t prio)
{
    unsigned n = intid / 32u;
    GICD_IPRIORITYR(intid) = prio;
    GICD_IGROUPR(n) |= (1u << (intid % 32u));
    GICD_ISENABLER(n) = (1u << (intid % 32u));
}

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

static uint32_t gic_hw_claim(hal_device_t *dev)
{
    (void)dev;
    return gic_read_iar1();
}

static void gic_hw_complete(hal_device_t *dev, uint32_t irq)
{
    (void)dev;
    gic_write_eoir1(irq);
}

static void gic_hw_set_threshold(hal_device_t *dev, uint32_t thr)
{
    (void)dev;
    gic_write_pmr(thr > 0xffu ? 0xffu : thr);
}

static uint32_t gic_hw_get_threshold(hal_device_t *dev)
{
    uint64_t v;
    (void)dev;
    __asm__ volatile("mrs %0, S3_0_C4_C6_0" : "=r"(v));
    return (uint32_t)v;
}

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

hal_device_t *drv_irqc_device(void)
{
    return &s_gic_dev;
}

extern void arm64_handle_timer(void);

/**
 * @brief IRQ 入口：claim → timer / 其它 dispatch → EOI
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
