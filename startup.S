/**
 * @file startup.S
 * @brief CG-RTOS RISC-V 启动与陷阱向量汇编入口。
 *
 * @details
 * 导出符号：_start, _start_secondary, trap_vector, start_first_task,
 * context_switch, _deadloop。
 *
 * 陷阱/上下文帧布局（272 字节 = 34 x 8）：
 *   0:ra  8:gp  16:tp  24:t0 ... 232:t6
 *   240:mcause  248:mepc  256:mstatus  264:mscratch
 *
 * SMP 启动流程：
 *   hart0: 清零 BSS → 写 g_boot_sync 魔数 → main() → 可选 MSIP 唤醒
 *   hart1: 阶段标记 → 设 SP → 轮询 g_boot_sync → secondary_main()
 *
 * QEMU 须将 hart1 置于镜像入口，例如：
 *   -device loader,addr=0xA0000000,cpu-num=1
 * Nuclei ROM 单独启动时常使 CPU1 停留在复位态。
 */

.section .text.init,"ax"
.global _start,_start_secondary,context_switch,start_first_task,trap_vector,_deadloop
.equ CSR_MCACHE_CTL,0x7CA
.equ BOOT_FLAG_MAGIC,0xCAFE5A5A

/**
 * @brief 主 hart 复位入口（hart0）或 secondary 分发点。
 *
 * hart0：关中断、设 mtvec、仅开 I-cache、清零 BSS、设 SP、
 * 写 g_boot_sync 后调用 main()。
 * hart1：跳转 _start_secondary。
 */
_start:
    csrw mie,zero
    csrw mip,zero
    la t0,trap_vector
    csrw mtvec,t0
    csrr t0,mhartid
    li t1,1
    beq t0,t1,_start_secondary
    /*
     * Invalidate boot magic BEFORE any work so a warm-RAM leftover 0xCAFE5A5A
     * cannot let hart1 race into C while we still wipe BSS.
     */
    la t0,g_boot_sync
    sw zero,0(t0)
    fence
    /*
     * Enable I-cache only. D-cache must stay off for MMIO (SysTimer/UART/CLINT);
     * otherwise mtimecmp stores are absorbed by cache and timer IRQs never fire.
     * Nuclei MCACHE_CTL: bit0=DC_EN, bit1=IC_EN.
     */
    li t0,0x2
    csrw CSR_MCACHE_CTL,t0
    la gp,__global_pointer$
    /* Wipe .bss and .sbss (linker merges both under _sbss.._ebss). */
    la t0,_sbss
    la t1,_ebss
    bgeu t0,t1,2f
1:  sd zero,0(t0)
    addi t0,t0,8
    blt t0,t1,1b
2:  la sp,_stack_top
    /* Release secondary only after BSS is clean. */
    la t0,g_boot_sync
    li t1,BOOT_FLAG_MAGIC
    sw t1,0(t0)
    fence
    csrr a0,mhartid
    la a1,_stack_top
    la a2,_end
    jal main
    j _deadloop

/**
 * @brief 从 hart 启动路径（hart1）。
 *
 * 在 g_hart1_stage 写入阶段面包屑，开 I-cache，设 gp/SP，
 * 短延时后轮询 g_boot_sync 魔数，匹配后调用 secondary_main()。
 */
_start_secondary:
    la t3,g_hart1_stage
    li t4,1
    sw t4,0(t3)
    fence
    li t0,0x2
    csrw CSR_MCACHE_CTL,t0
    fence
    la gp,__global_pointer$
    la sp,_stack_top_core1
    li t4,2
    sw t4,0(t3)
    fence
    /* Wait for hart0 to finish BSS wipe + store magic (may race briefly). */
    li t0,800000
.Ldelay1:
    addi t0,t0,-1
    bnez t0,.Ldelay1
    fence
    li t4,3
    sw t4,0(t3)
    fence
    la t0,g_boot_sync
    li t2,BOOT_FLAG_MAGIC
    /*
     * Compare with lwu: magic has bit31 set, so plain lw sign-extends to
     * 0xFFFFFFFFCAFE5A5A while li leaves 0x00000000CAFE5A5A — bne never matches.
     */
.Lpoll_sync:
    lwu t1,0(t0)
    bne t1,t2,.Lpoll_sync
    fence
    li t4,4
    sw t4,0(t3)
    fence
    csrr a0,mhartid
    jal secondary_main
    j _deadloop

.align 6
/**
 * @brief 机器模式统一陷阱向量（中断与异常）。
 *
 * 保存 272 字节上下文帧，按 mcause 分发：IPI(3)、定时器(7)、
 * 外部(11)、ecall 自愿让出、其它异常转 riscv_handle_exception。
 * 若 g_yield_pending 置位则调用 cgrtos_sched_switch_from_trap 后 mret。
 */
trap_vector:
    addi sp,sp,-272
    sd ra,0(sp)
    sd gp,8(sp)
    sd tp,16(sp)
    sd t0,24(sp)
    sd t1,32(sp)
    sd t2,40(sp)
    sd s0,48(sp)
    sd s1,56(sp)
    sd a0,64(sp)
    sd a1,72(sp)
    sd a2,80(sp)
    sd a3,88(sp)
    sd a4,96(sp)
    sd a5,104(sp)
    sd a6,112(sp)
    sd a7,120(sp)
    sd s2,128(sp)
    sd s3,136(sp)
    sd s4,144(sp)
    sd s5,152(sp)
    sd s6,160(sp)
    sd s7,168(sp)
    sd s8,176(sp)
    sd s9,184(sp)
    sd s10,192(sp)
    sd s11,200(sp)
    sd t3,208(sp)
    sd t4,216(sp)
    sd t5,224(sp)
    sd t6,232(sp)
    csrr t0,mcause
    sd t0,240(sp)
    csrr t1,mepc
    sd t1,248(sp)
    csrr t2,mstatus
    sd t2,256(sp)
    csrr t3,mscratch
    sd t3,264(sp)
    la gp,__global_pointer$

    csrr t0,mcause
    bgez t0,is_exception
    li t1,0x7fffffffffffffff
    and t0,t0,t1
    li t1,3
    beq t0,t1,handle_ipi
    li t1,7
    beq t0,t1,handle_timer
    li t1,11
    beq t0,t1,handle_external
    j trap_done

handle_ipi:
    csrr t0,mhartid
    li t1,0x18031000
    slli t2,t0,2
    add t1,t1,t2
    sw zero,0(t1)
    mv a0,sp
    jal riscv_handle_ipi
    j trap_done

handle_timer:
    mv a0,sp
    jal riscv_handle_timer
    j trap_done

handle_external:
    mv a0,sp
    jal riscv_handle_external
    j trap_done

is_exception:
    csrr t0,mcause
    li t1,11
    beq t0,t1,handle_ecall
    mv a0,sp
    csrr a1,mcause
    csrr a2,mepc
    jal riscv_handle_exception
    j _deadloop

/* Voluntary yield: ecall from M-mode */
handle_ecall:
    ld t0,248(sp)
    addi t0,t0,4
    sd t0,248(sp)
    csrr t0,mhartid
    la t1,g_yield_pending
    add t1,t1,t0
    li t2,1
    sb t2,0(t1)
    j trap_done

trap_done:
    csrr t0,mhartid
    la t1,g_yield_pending
    add t1,t1,t0
    lbu t2,0(t1)
    beqz t2,trap_restore
    sb zero,0(t1)
    mv a0,sp
    jal cgrtos_sched_switch_from_trap
    mv sp,a0

/**
 * @brief 启动第一个任务：将 a0 作为新 SP 并走 trap_restore 返回用户/任务上下文。
 * @note 仅首次启动强制 MPIE=1；普通 trap 恢复使用帧内保存的 mstatus。
 */
.global start_first_task
start_first_task:
    mv sp,a0
    /* First task frame must return with interrupts enabled. */
    ld t0,256(sp)
    ori t0,t0,0x80
    sd t0,256(sp)
    j trap_restore

trap_restore:
    ld t0,264(sp)
    csrw mscratch,t0
    ld t0,256(sp)
    csrw mstatus,t0
    ld t0,248(sp)
    csrw mepc,t0
    ld ra,0(sp)
    ld gp,8(sp)
    ld tp,16(sp)
    ld t0,24(sp)
    ld t1,32(sp)
    ld t2,40(sp)
    ld s0,48(sp)
    ld s1,56(sp)
    ld a0,64(sp)
    ld a1,72(sp)
    ld a2,80(sp)
    ld a3,88(sp)
    ld a4,96(sp)
    ld a5,104(sp)
    ld a6,112(sp)
    ld a7,120(sp)
    ld s2,128(sp)
    ld s3,136(sp)
    ld s4,144(sp)
    ld s5,152(sp)
    ld s6,160(sp)
    ld s7,168(sp)
    ld s8,176(sp)
    ld s9,184(sp)
    ld s10,192(sp)
    ld s11,200(sp)
    ld t3,208(sp)
    ld t4,216(sp)
    ld t5,224(sp)
    ld t6,232(sp)
    addi sp,sp,272
    mret

/* Kept for ABI compatibility — redirects to ecall yield */
context_switch:
    j cgrtos_sched_yield

/** @brief main/secondary_main 返回后的 WFI 死循环。 */
_deadloop:
    wfi
    j _deadloop
