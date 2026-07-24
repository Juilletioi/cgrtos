/**
 * @file exception.c
 * @brief AArch64 sync / invalid 异常入口
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * - arm64_handle_sync：处理 SVC（自愿 yield）及其它同步异常诊断。
 * - arm64_handle_invalid：非法向量入口，打印 ESR/ELR 后 WFI 挂起。
 * - 诊断输出走 drv_uart_early_puts，不依赖 printf。
 */

#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"

extern volatile uint8_t g_yield_pending[CONFIG_NUM_CORES];

/**
 * @brief 同步异常入口：SVC 自愿让出，或其它 ESR 诊断挂起
 * @details
 * 1. 读 ESR_EL1 / FAR_EL1，取 EC（bits[31:26]）。
 * 2. EC=0x15（AArch64 SVC）：置本 CPU g_yield_pending=1 后返回；
 *    ELR 已指向 SVC 下一条指令，不可再 +4。
 * 3. 其它 EC：cgrtos_isr_enter，early UART 打印 ESR/FAR，isr_exit，然后死循环 WFI。
 * @param[in] f 异常栈帧指针；本实现未使用
 * @return 无
 * @retval 无
 * @note 由向量表 sync 入口调用
 * @warning 非 SVC 路径不可恢复，仅供调试
 * @attention ✅ ISR-safe；✅ 可能引起上下文切换（SVC 置 g_yield_pending）；非 SVC 永久阻塞（WFI）
 */
void arm64_handle_sync(uint64_t *f)
{
    uint64_t esr, far;
    unsigned cpu;
    uint32_t ec;

    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    ec = (uint32_t)((esr >> 26) & 0x3fu);

    /* SVC from AArch64 (EC=0x15) — voluntary yield.
     * ELR_EL1 already points at the instruction after SVC; do not +4
     * (that would skip the function epilogue / ret). */
    if (ec == 0x15u) {
        cpu = arch_cpu_id();
        g_yield_pending[cpu] = 1;
        (void)f;
        return;
    }

    cgrtos_isr_enter();
    drv_uart_early_puts("\n[ARM64 SYNC] ESR=");
    /* Minimal hex dump without printf dependency in early path */
    {
        char buf[17];
        int i;
        for (i = 15; i >= 0; i--) {
            unsigned n = (unsigned)((esr >> (i * 4)) & 0xf);
            buf[15 - i] = (char)(n < 10 ? '0' + n : 'a' + n - 10);
        }
        buf[16] = 0;
        drv_uart_early_puts(buf);
    }
    drv_uart_early_puts(" FAR=");
    {
        char buf[17];
        int i;
        for (i = 15; i >= 0; i--) {
            unsigned n = (unsigned)((far >> (i * 4)) & 0xf);
            buf[15 - i] = (char)(n < 10 ? '0' + n : 'a' + n - 10);
        }
        buf[16] = 0;
        drv_uart_early_puts(buf);
    }
    drv_uart_early_puts("\n");
    cgrtos_isr_exit();
    (void)f;
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/**
 * @brief 非法 / 未实现向量入口：打印 ESR/ELR 后永久挂起
 * @details
 * 1. 读 ESR_EL1 / ELR_EL1。
 * 2. drv_uart_early_puts 输出十六进制诊断信息。
 * 3. 死循环 WFI，不返回。
 * @param[in] f 异常栈帧指针；本实现未使用
 * @return 无
 * @retval 无
 * @note 仅供错误向量诊断；正常运行不应进入
 * @warning 不可恢复；UART 输出无锁、轮询阻塞
 * @attention ✅ ISR-safe；✅ 永久阻塞（WFI 死循环）
 */
void arm64_handle_invalid(uint64_t *f)
{
    uint64_t esr, elr;
    (void)f;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    drv_uart_early_puts("\n[ARM64 INVALID EXC] ESR=");
    {
        char buf[17];
        int i;
        for (i = 15; i >= 0; i--) {
            unsigned n = (unsigned)((esr >> (i * 4)) & 0xf);
            buf[15 - i] = (char)(n < 10 ? '0' + n : 'a' + n - 10);
        }
        buf[16] = 0;
        drv_uart_early_puts(buf);
    }
    drv_uart_early_puts(" ELR=");
    {
        char buf[17];
        int i;
        for (i = 15; i >= 0; i--) {
            unsigned n = (unsigned)((elr >> (i * 4)) & 0xf);
            buf[15 - i] = (char)(n < 10 ? '0' + n : 'a' + n - 10);
        }
        buf[16] = 0;
        drv_uart_early_puts(buf);
    }
    drv_uart_early_puts("\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}
