/**
 * @file task_stack.c
 * @brief RISC-V 任务 trap 栈帧初始化
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * - 在任务栈顶构造与 trap 返回路径匹配的 34×uint64_t 帧。
 * - sp[1]=gp（__global_pointer$），sp[8]=a0(arg)，sp[30]=ra，sp[31]=mepc(fn)，
 *   sp[32]=mstatus（MPP=M, MPIE=1, MIE=0），sp[33]=pad。
 * - 可选 CONFIG_CHECK_STACK_OVERFLOW：整栈填 0xA5 并写 canary。
 */

#include "../../kernel/cgrtos.h"
#include <string.h>

extern char __global_pointer$;

/**
 * @brief 为新建任务填充 RISC-V 初始 trap 栈帧
 * @details
 * 1. 从 task->stack 顶向下对齐并预留 34 个 uint64_t。
 * 2. 若开启栈溢出检测：memset 整栈 0xA5 并写 STACK_CANARY_VALUE；
 *    否则仅清零帧区域。
 * 3. 写入 gp、a0=arg、ra=0、mepc=fn、mstatus=0x1880；task->sp = 帧基址。
 * @param[in,out] task 任务控制块；写 sp 与可选 canary
 * @param[in]     fn   任务入口函数
 * @param[in]     arg  传给 fn 的参数（经 a0）
 * @return 无
 * @retval 无
 * @note 须在任务首次调度前调用；帧布局与 arch 下 task_stack / startup.S 一致
 * @warning task / fn 不可为 NULL；栈大小须足以容纳 trap 帧
 * @attention ❌ ISR-safe；❌ 不阻塞、不引起上下文切换
 */
void arch_task_stack_init(struct cgrtos_task *task, void (*fn)(void *), void *arg)
{
    uint64_t *sp = (uint64_t *)((uint8_t *)task->stack + sizeof(task->stack) - 16);
    sp -= 34;

#if CONFIG_CHECK_STACK_OVERFLOW
    memset(task->stack, 0xA5, sizeof(task->stack));
    task->stack[0] = STACK_CANARY_VALUE;
#else
    memset(sp, 0, 34 * sizeof(uint64_t));
#endif

    sp[1]  = (uint64_t)(uintptr_t)&__global_pointer$;
    sp[8]  = (uint64_t)(uintptr_t)arg;
    sp[30] = 0;
    sp[31] = (uint64_t)(uintptr_t)fn;
    sp[32] = 0x1880; /* MPP=M, MPIE=1, MIE=0 */
    sp[33] = 0;
    task->sp = sp;
}
