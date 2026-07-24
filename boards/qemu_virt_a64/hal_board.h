/**
 * @file boards/qemu_virt_a64/hal_board.h
 * @brief QEMU virt AArch64 板级 MMIO（PL011 + GICv3 + RAM）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * - UART: PL011 @ 0x09000000
 * - IRQC: GICv3（GICD @ 0x08000000，GICR @ 0x080A0000）
 * - Timer: 虚拟定时器 PPI INTID 27
 * - DRAM @ 0x40000000（256MiB）
 * QEMU: -M virt -cpu cortex-a53|… -kernel cgrtos.elf
 */
#ifndef HAL_BOARD_QEMU_VIRT_A64_H
#define HAL_BOARD_QEMU_VIRT_A64_H

/**
 * @brief 板级短名字符串（诊断/打印）
 * @warning 无运行时副作用（字符串常量）
 */
#define HAL_BOARD              "qemu_virt_a64"

/* DRAM */
/**
 * @brief DRAM 物理起始地址
 * @warning 无运行时副作用（地址常量）
 */
#define HAL_BOARD_RAM_BASE          0x40000000ULL
/**
 * @brief DRAM 容量（字节）
 * @warning 无运行时副作用（容量常量）；须与 QEMU 内存配置一致
 */
#define HAL_BOARD_RAM_SIZE          (256ULL * 1024 * 1024)

/* PL011 UART0 */
/**
 * @brief PL011 UART0 MMIO 基址
 * @warning 无运行时副作用（地址常量）
 */
#define HAL_BOARD_UART0_BASE        0x09000000ULL
/**
 * @brief 控制台 UART 基址别名（等同 UART0）
 * @warning 仅为别名，无额外运行时副作用
 */
#define HAL_BOARD_UART_BASE         HAL_BOARD_UART0_BASE
/**
 * @brief 本板 UART 为 PL011 风格
 * @warning 无运行时副作用（编译期选型常量）
 */
#define HAL_BOARD_UART_KIND_PL011   1
/**
 * @brief PL011 UART0 在 GIC 上的 SPI INTID
 * @warning 无运行时副作用（中断号常量）
 */
#define HAL_BOARD_UART_IRQ          33
/**
 * @brief 虚拟定时器 PPI INTID（GICv3）
 * @warning 无运行时副作用（中断号常量）
 */
#define HAL_BOARD_TIMER_IRQ         27

/* GICv3 (QEMU virt defaults) */
/**
 * @brief GIC Distributor（GICD）MMIO 基址
 * @warning 无运行时副作用（地址常量）
 */
#define HAL_BOARD_GICD_BASE         0x08000000ULL
/**
 * @brief GIC Redistributor（GICR）MMIO 基址
 * @warning 无运行时副作用（地址常量）
 */
#define HAL_BOARD_GICR_BASE         0x080A0000ULL

/* Unused RISC-V symbols — keep HAL compile paths quiet if referenced */
/**
 * @brief RISC-V CLINT 占位（本板未使用，恒为 0）
 * @warning 无运行时副作用；误用于 MMIO 写将访问空地址
 */
#define HAL_BOARD_CLINT_BASE        0
/**
 * @brief RISC-V PLIC 占位基址（本板未使用，恒为 0）
 * @warning 无运行时副作用；误用于 MMIO 写将访问空地址
 */
#define HAL_BOARD_PLIC_BASE         0
/**
 * @brief RISC-V PLIC priority 区占位（本板未使用）
 * @warning 无运行时副作用
 */
#define HAL_BOARD_PLIC_PRIORITY_BASE 0
/**
 * @brief RISC-V PLIC pending 区占位（本板未使用）
 * @warning 无运行时副作用
 */
#define HAL_BOARD_PLIC_PENDING_BASE  0
/**
 * @brief RISC-V PLIC enable 区占位（本板未使用）
 * @warning 无运行时副作用
 */
#define HAL_BOARD_PLIC_ENABLE_BASE   0
/**
 * @brief RISC-V PLIC threshold 区占位（本板未使用）
 * @warning 无运行时副作用
 */
#define HAL_BOARD_PLIC_THRESHOLD_BASE 0
/**
 * @brief RISC-V PLIC claim 区占位（本板未使用）
 * @warning 无运行时副作用
 */
#define HAL_BOARD_PLIC_CLAIM_BASE    0
/**
 * @brief 非 SiFive UART（本板为 PL011）
 * @warning 无运行时副作用（编译期选型常量）
 */
#define HAL_BOARD_UART_KIND_SIFIVE  0
/**
 * @brief 非 NS16550 UART（本板为 PL011）
 * @warning 无运行时副作用（编译期选型常量）
 */
#define HAL_BOARD_UART_KIND_NS16550 0

#ifndef CONFIG_NUCLEI_MCACHE
/**
 * @brief 关闭 Nuclei mcache 路径（本板为 AArch64）
 * @warning 无运行时副作用（编译期开关）
 */
#define CONFIG_NUCLEI_MCACHE        0
#endif

/**
 * @brief 板级 IPI 清除占位（AArch64 virt 无 CLINT MSIP）
 * @details 本板 IPI 由 GICv3 SGI 等路径处理；此内联为空操作，避免通用代码缺符号。
 * @param[in] cpu 目标 CPU 编号（本实现忽略）
 * @return 无
 * @retval 无
 * @note 真实清 IPI 应走 arch IPI 驱动 / GIC SGI 路径
 * @warning 调用本函数不会清除任何硬件 pending
 * @attention ✅ ISR；❌ 不阻塞
 * @internal
 */
static inline void board_ipi_clear(unsigned cpu)
{
    (void)cpu;
}

#endif
