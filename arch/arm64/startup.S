/**
 * @file startup.S
 * @brief CG-RTOS AArch64 启动与异常向量（QEMU virt / EL1）
 *
 * 异常帧 272 字节（34 x 8），与 arch_task_stack_init 一致：
 *   [0..30]  x0..x30
 *   [31]     SPSR_EL1
 *   [32]     ELR_EL1
 *   [33]     保留
 *
 * 自愿让出：svc 0 → sync 异常 → 置 yield → 可选 switch。
 * 定时器：CNTV IRQ（PPI 27）经 GIC → arm64_handle_timer。
 *
 * Note: aarch64 GNU as treats '#' as line comment — avoid '#' immediates.
 */
.section .text.boot,"ax"
.global _start, _start_secondary, context_switch, start_first_task
.global vectors, _deadloop
.equ CORE_STACK_SHIFT, 14
.equ MAX_CORES, 4
.equ FRAME_SIZE, 272

_start:
    /* Drop EL2 → EL1 if needed (QEMU virt may start in either). */
    mrs x0, CurrentEL
    lsr x0, x0, 2
    cmp x0, 2
    b.ne 1f
    /* Allow EL1 timer access; zero virtual offset */
    mov x0, 3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    mov x0, 1
    lsl x0, x0, 31          /* HCR_EL2.RW */
    msr hcr_el2, x0
    movz x0, 0x3c5          /* EL1h, DAIF masked */
    msr spsr_el2, x0
    adr x0, 1f
    msr elr_el2, x0
    eret
1:
    msr daifset, 0xf
    adr x0, vectors
    msr vbar_el1, x0

    mrs x0, mpidr_el1
    and x0, x0, 0xff
    cbnz x0, _start_secondary

    /* Invalidate boot magic before BSS wipe */
    adrp x0, g_boot_sync
    add x0, x0, :lo12:g_boot_sync
    str wzr, [x0]
    dsb sy

    /* Clear BSS */
    adrp x0, _sbss
    add x0, x0, :lo12:_sbss
    adrp x1, _ebss
    add x1, x1, :lo12:_ebss
    b 3f
2:  str xzr, [x0], 8
3:  cmp x0, x1
    b.lo 2b

    adrp x0, _stack_top
    add sp, x0, :lo12:_stack_top

    adrp x0, g_boot_sync
    add x0, x0, :lo12:g_boot_sync
    movz w1, 0x5A5A
    movk w1, 0xCAFE, lsl 16
    str w1, [x0]
    dsb sy

    mov x0, 0
    adrp x1, _stack_top
    add x1, x1, :lo12:_stack_top
    adrp x2, _end
    add x2, x2, :lo12:_end
    bl main
    b _deadloop

_start_secondary:
    mrs x5, mpidr_el1
    and x5, x5, 0xff
    cmp x5, MAX_CORES
    b.hs _deadloop

    adrp x3, g_hart_stage
    add x3, x3, :lo12:g_hart_stage
    lsl x4, x5, 2
    add x3, x3, x4
    mov w4, 1
    str w4, [x3]
    dsb sy

    adrp x0, _stack_top
    add sp, x0, :lo12:_stack_top
    lsl x1, x5, CORE_STACK_SHIFT
    add sp, sp, x1

    mov w4, 2
    str w4, [x3]
    dsb sy

    movz x0, 0xC350
    movk x0, 0xC, lsl 16    /* 800000 */
4:  subs x0, x0, 1
    b.ne 4b

    mov w4, 3
    str w4, [x3]
    dsb sy

    adrp x0, g_boot_sync
    add x0, x0, :lo12:g_boot_sync
    movz w2, 0x5A5A
    movk w2, 0xCAFE, lsl 16
5:  ldr w1, [x0]
    cmp w1, w2
    b.ne 5b
    dsb sy

    mov w4, 4
    str w4, [x3]
    dsb sy

    mov x0, x5
    bl secondary_main
    b _deadloop

/* ---- Exception vectors (2KB aligned) ------------------------------------ */
.align 11
vectors:
    /* Current EL with SP0 */
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid
    /* Current EL with SPx (EL1h) — our path */
    .align 7
    b exc_sync
    .align 7
    b exc_irq
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid
    /* Lower EL AArch64 */
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid
    /* Lower EL AArch32 */
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid
    .align 7
    b exc_invalid

/* Save GPRs + SPSR + ELR onto SP; x0 = frame pointer for C handlers */
.macro SAVE_CONTEXT
    sub sp, sp, FRAME_SIZE
    stp x0, x1, [sp, 0]
    stp x2, x3, [sp, 16]
    stp x4, x5, [sp, 32]
    stp x6, x7, [sp, 48]
    stp x8, x9, [sp, 64]
    stp x10, x11, [sp, 80]
    stp x12, x13, [sp, 96]
    stp x14, x15, [sp, 112]
    stp x16, x17, [sp, 128]
    stp x18, x19, [sp, 144]
    stp x20, x21, [sp, 160]
    stp x22, x23, [sp, 176]
    stp x24, x25, [sp, 192]
    stp x26, x27, [sp, 208]
    stp x28, x29, [sp, 224]
    str x30, [sp, 240]
    mrs x0, spsr_el1
    str x0, [sp, 248]
    mrs x0, elr_el1
    str x0, [sp, 256]
    str xzr, [sp, 264]
    mov x0, sp
.endm

.macro RESTORE_CONTEXT
    ldr x0, [sp, 248]
    /* Tasks always resume with IRQs enabled (critical sections use DAIF in C). */
    mov x1, 1
    lsl x1, x1, 7
    bic x0, x0, x1
    msr spsr_el1, x0
    ldr x0, [sp, 256]
    msr elr_el1, x0
    ldp x0, x1, [sp, 0]
    ldp x2, x3, [sp, 16]
    ldp x4, x5, [sp, 32]
    ldp x6, x7, [sp, 48]
    ldp x8, x9, [sp, 64]
    ldp x10, x11, [sp, 80]
    ldp x12, x13, [sp, 96]
    ldp x14, x15, [sp, 112]
    ldp x16, x17, [sp, 128]
    ldp x18, x19, [sp, 144]
    ldp x20, x21, [sp, 160]
    ldp x22, x23, [sp, 176]
    ldp x24, x25, [sp, 192]
    ldp x26, x27, [sp, 208]
    ldp x28, x29, [sp, 224]
    ldr x30, [sp, 240]
    add sp, sp, FRAME_SIZE
.endm

exc_sync:
    SAVE_CONTEXT
    bl arm64_handle_sync
    b trap_done

exc_irq:
    SAVE_CONTEXT
    bl arm64_handle_irq
    b trap_done

exc_invalid:
    SAVE_CONTEXT
    bl arm64_handle_invalid
    b _deadloop

trap_done:
    mrs x1, mpidr_el1
    and x1, x1, 0xff
    adrp x2, g_yield_pending
    add x2, x2, :lo12:g_yield_pending
    add x2, x2, x1
    ldrb w3, [x2]
    cbz w3, trap_restore
    strb wzr, [x2]
    mov x0, sp
    bl cgrtos_sched_switch_from_trap
    mov sp, x0

.global start_first_task
start_first_task:
    mov sp, x0
    /* First task: clear I (bit7) in SPSR so IRQs enable on ERET */
    ldr x1, [sp, 248]
    mov x2, 1
    lsl x2, x2, 7
    bic x1, x1, x2
    str x1, [sp, 248]
    b trap_restore

trap_restore:
    RESTORE_CONTEXT
    eret

context_switch:
    b cgrtos_sched_yield

_deadloop:
    wfi
    b _deadloop
