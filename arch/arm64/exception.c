/**
 * @file exception.c
 * @brief AArch64 sync / invalid 异常入口
 */
#include "../../kernel/cgrtos.h"
#include "../../hal/hal_drv.h"

extern volatile uint8_t g_yield_pending[CONFIG_NUM_CORES];

/**
 * @brief SVC / 其它 sync：自愿让出已在 C 侧置 g_yield_pending；此处处理 ESR
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
