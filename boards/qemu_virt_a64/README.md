# ARM Cortex-A55 / A75 class on QEMU virt (AArch64)

QEMU 6.2 has no `cortex-a55` / `cortex-a75`; use:

| Wanted | QEMU `-cpu` |
|--------|-------------|
| A55-class | `cortex-a53` |
| A75-class | `cortex-a72` |

```bash
export PATH=/home/cong/cgrtos-tools/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/home/cong/cgrtos-tools/root/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

./scripts/cgrtos.sh demo --board qemu_virt_a64 --cores 1 --cpu cortex-a53
./scripts/cgrtos.sh demo --board qemu_virt_a64 --cores 1 --cpu cortex-a72
```

See [docs/PORTING.md](../../docs/PORTING.md).
