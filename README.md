# CG-RTOS v5.3

Freestanding **multi-arch** SMP RTOS（RISC-V 64 + AArch64），面向 Nuclei / SiFive / QEMU virt 等目标。

## Quick start

```bash
./scripts/cgrtos.sh test --board riscv_virt --cores 2
./scripts/cgrtos.sh stress --board riscv_virt --cores 2
./scripts/cgrtos.sh demo --board nuclei_evalsoc --cores 2
./scripts/port-check.sh                    # RV + ARM64 门禁
CGRTOS_PORT_SMOKE=1 ./scripts/port-check.sh

# ARM64（A55/A75 类：cortex-a53 / a72）
export PATH=/home/cong/cgrtos-tools/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/home/cong/cgrtos-tools/root/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
./scripts/cgrtos.sh test --board qemu_virt_a64 --cores 1 --cpu cortex-a53 --clean
```

Docs:

- Porting: [docs/PORTING.md](docs/PORTING.md)
- Scripts: [docs/SCRIPTS.md](docs/SCRIPTS.md)
- Guide: [docs/USER_GUIDE.md](docs/USER_GUIDE.md)
- Architecture: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- HAL: [docs/HAL.md](docs/HAL.md)

## Features

- Multi-policy preemptive sched: RR / Priority / CFS / **MC-EDF** / Hybrid
- SMP affinity, weighted LB, IPI（RISC-V；ARM64 单核首版）
- Unified **HAL** + `boards/{BOARD}/` BSP + `kernel/arch_port.h` CPU 移植层
- Sem / mutex / queue / event / notify, StreamBuffer / MessageBuffer, QueueSet
- Soft timers, TLSF heap, RAM filesystem
- `scripts/cgrtos.sh` + `scripts/port-check.sh`

## Layout

```
kernel/          调度、任务、IPC…（架构无关；经 arch_port / HAL）
kernel/arch_port.h   CPU：核号 / irq / yield / wfi / IPI / 栈帧
hal/             设备框架与用户 HAL API
arch/riscv/      UART / CLINT / PLIC / IPI / startup
arch/arm64/      PL011 / CNTV / GIC / startup
boards/          板级 MMIO、board.mk、link.lds
build/{BOARD}/   每板对象文件
tests/           test / stress / bench
scripts/         cgrtos.sh、port-check.sh、gdbinit
docs/            PORTING、SCRIPTS、ARCHITECTURE、HAL…
```
