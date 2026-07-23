/**
 * @file task_stack.c
 * @brief RISC-V 任务 trap 栈帧初始化
 */
#include "../../kernel/cgrtos.h"
#include <string.h>

extern char __global_pointer$;

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
