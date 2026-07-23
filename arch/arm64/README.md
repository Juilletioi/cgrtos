# ARM64 / QEMU virt (A53≈A55, A72≈A75)

Board: `boards/qemu_virt_a64/`

| File | Role |
|------|------|
| `startup.S` | EL2→EL1, VBAR vectors, context switch |
| `gic.c` | GICv3 distributor + redistributor |
| `timer.c` | CNTV_CTL / CNTV_TVAL (drv_clint_device) |
| `uart_pl011.c` | PL011 @ 0x09000000 |
| `arch.c` / `ipic.c` / `exception.c` / `task_stack.c` | CPU / IPI stub / sync / stack |

```bash
# Host tools (this tree uses cgrtos-tools if present)
export PATH=/home/cong/cgrtos-tools/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/home/cong/cgrtos-tools/root/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

make BOARD=qemu_virt_a64 CORES=1 CPU=cortex-a53 APP=demo
./scripts/cgrtos.sh demo --board qemu_virt_a64 --cores 1 --cpu cortex-a53
# A75-class stand-in on QEMU 6.2:
./scripts/cgrtos.sh demo --board qemu_virt_a64 --cores 1 --cpu cortex-a72
```

See [docs/PORTING.md](../../docs/PORTING.md).
