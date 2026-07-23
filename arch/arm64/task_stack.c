/**
 * @file task_stack.c
 * @brief AArch64 任务异常栈帧初始化（与 startup.S FRAME_SIZE=272 一致）
 */
#include "../../kernel/cgrtos.h"
#include <string.h>

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

    sp[0]  = (uint64_t)(uintptr_t)arg;          /* x0 */
    sp[30] = 0;                                 /* x30 / LR */
    sp[31] = 0x5;                               /* SPSR: EL1h, IRQs unmasked */
    sp[32] = (uint64_t)(uintptr_t)fn;           /* ELR */
    sp[33] = 0;
    task->sp = sp;
}
