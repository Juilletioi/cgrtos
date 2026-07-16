/**
 * @file arch.c
 * @brief RISC-V 架构层初始化（Nuclei UX900）。
 *
 * 在 BSS 清零之后、调度器启动之前调用，配置机器级中断使能。
 */

#include "../../kernel/cgrtos.h"

/**
 * @brief 初始化 RISC-V 机器级中断。
 *
 * @details
 * 1. Nuclei UX900 特定初始化（栈、向量表等）已在 startup.S 中完成。
 * 2. 本函数在 BSS 清零之后、调度器启动之前被调用。
 * 3. 通过 set_csr_bits(mie, 0x888) 一次性打开三类 M 模式中断：
 *    - bit 3 (0x008)：MSIE — 机器软件中断（IPI）；
 *    - bit 7 (0x080)：MTIE — 机器定时器中断（tick）；
 *    - bit 11 (0x800)：MEIE — 机器外部中断（PLIC）。
 *
 * @note 具体外设（CLINT/PLIC）的进一步配置在 cgrtos_clint_init / cgrtos_plic_init 中完成。
 */
void cgrtos_arch_init(void){
    // Nuclei UX900 specific initialization done in startup.S
    // This is called after BSS clear but before scheduler starts
    /* 3. 打开 MSIE/MTIE/MEIE 三类 M 模式中断 */
    set_csr_bits(mie, 0x888); /* MEIE | MTIE | MSIE */
}
