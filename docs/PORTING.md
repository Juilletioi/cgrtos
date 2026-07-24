# 多 CPU / 多板级移植指南

CG-RTOS 通过 **板级 BSP**（`boards/{BOARD}/`）+ **架构实现**（`arch/<ARCH>/`）+ **CPU 移植头**（`kernel/arch_port.h`）支持多架构。内核业务代码不得直接写 CSR / DAIF / `svc` / `ecall`。

## 分层

```
应用 (demo / test / cli)
        │
        ▼
  内核 API (cgrtos.h)     ←── 仅用 arch_* / HAL
        │
   ┌────┴────┐
   ▼         ▼
arch_port.h   hal_*
   │         │
   ▼         ▼
arch/<ARCH>  drv_timer / drv_irqc / drv_uart / drv_ipi
   │         │
   └────┬────┘
        ▼
 boards/{BOARD}/hal_board.h + board.mk + link.lds
```

## 已支持并在 QEMU 可跑

| BOARD | ARCH | CPU 示例 | QEMU |
|-------|------|----------|------|
| `nuclei_evalsoc`（默认） | riscv | `nuclei-ux900fd` / `nuclei-nx900fd` / `nuclei-nx600fd` | Nuclei QEMU |
| `riscv_virt` | riscv | `rv64` / `sifive-u54` | `qemu-system-riscv64` |
| `sifive_u` | riscv | `sifive-u54`（`CORES>=2`） | 同上 |
| `qemu_virt_a64` | arm64 | `cortex-a53`（≈A55）/ `cortex-a72`（≈A75） | `qemu-system-aarch64` |

## 命令示例

```bash
# 一键门禁（RV virt test/stress + ARM64 test/stress）
./scripts/port-check.sh
CGRTOS_PORT_SMOKE=1 ./scripts/port-check.sh   # 另加 nuclei/sifive/a72 demo

# 单板
./scripts/cgrtos.sh test   --board riscv_virt --cores 2 --clean
./scripts/cgrtos.sh stress --board riscv_virt --cores 2

export PATH=/home/cong/cgrtos-tools/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/home/cong/cgrtos-tools/root/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
./scripts/cgrtos.sh test --board qemu_virt_a64 --cores 1 --cpu cortex-a53 --clean
./scripts/cgrtos.sh demo --board qemu_virt_a64 --cores 1 --cpu cortex-a72
```

对象文件在 `build/{BOARD}/`；固件仍输出到仓库根目录 `cgrtos.elf` / `cgrtos.bin`。

## 必实现：`arch_port.h` API

| API | 作用 |
|-----|------|
| `arch_cpu_id()` | 逻辑核号 |
| `arch_irq_save` / `arch_irq_restore` | 关/开可屏蔽 IRQ |
| `arch_yield_trap()` | 自愿陷入（`ecall` / `svc #0`） |
| `arch_cpu_wait()` | idle / halt（`wfi`） |
| `arch_cpu_enable_ipi()` | 次核开 IPI 接收 |
| `arch_task_stack_init()` | 任务初始栈帧（`arch/<arch>/task_stack.c`） |

内核包装：`cgrtos_irq_save` → `arch_irq_save`；`cgrtos_sched_yield` → `arch_yield_trap`。

## 必实现：驱动导出

| 中性名 | 兼容旧名 | 角色 |
|--------|----------|------|
| `drv_timer_device` | `drv_clint_device` | 系统定时器 + `hal_mtime_read` |
| `drv_irqc_device` | `drv_plic_device` | 外部中断控制器 |
| `drv_uart_device` / `drv_uart_early_*` | — | 控制台 |
| `drv_ipi_device` | — | 核间中断 |
| `drv_cpu_device` | — | 早期 CPU 初始化 |

Trap 入口（RISC-V `startup.S`）：`arch_handle_timer` / `ipi` / `external` / `exception`。  
旧名 `riscv_handle_*` 为宏别名。

## 板级文件

```
boards/<board>/
  hal_board.h   # MMIO / IRQ 号 / HAL_BOARD
  board.mk      # BOARD_ARCH、CROSS、QEMU、TIMER_HZ、UART、DELAY_BUSY_US
  link.lds      # RAM ORIGIN / LENGTH
arch/<arch>/startup.S   # 默认启动与向量（可用 BOARD_STARTUP 覆盖）
```

常用板级宏：

- `CONFIG_DELAY_BUSY_US=0`：纯 tick 延时（`qemu_virt_a64` 已设；避免忙等饿死）。
- `BOARD_QEMU_LOAD`：`bios_bin` | `elf` | `kernel`。
- `BOARD_QEMU_MAX_CORES` / `MIN_CORES`。

## ARM64 注意

- `-M virt,gic-version=3`，`-kernel cgrtos.elf`。
- 外设：PL011、GICv3、CNTV。
- 当前 `BOARD_QEMU_MAX_CORES=1`（无完整 PSCI/SGI SMP）。
- QEMU 6.2 无 a55/a75 时用 a53/a72。
- **SVC：ELR 已指向下一条指令，处理函数勿再 +4。**

## 新增一块板 / 一种 CPU

1. 复制相近 `boards/<ref>/`，改 `hal_board.h` / `board.mk` / `link.lds`。
2. 若新 ISA：在 `arch_port.h` 增加分支，新建 `arch/<arch>/`（startup、task_stack、timer、irqc、uart、ipi）。
3. `./scripts/cgrtos.sh demo --board <name> --cores 1 --clean`。
4. 再跑 `test` / `stress`。

## RISC-V CSR

`read_csr` 等宏在 [`arch/riscv/riscv_csr.h`](../arch/riscv/riscv_csr.h)；经 `cgrtos.h` 在 `__riscv` 下兼容包含。新代码优先 `arch_port.h`。
