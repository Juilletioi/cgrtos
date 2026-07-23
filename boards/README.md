# CG-RTOS boards

| Board | ARCH | CPUs (QEMU) | Notes |
|-------|------|-------------|-------|
| [nuclei_evalsoc](nuclei_evalsoc/) | riscv | nuclei-ux900fd, nx900fd, nx600fd, … | 芯来 evalsoc；默认 |
| [riscv_virt](riscv_virt/) | riscv | rv64, sifive-u54 | 通用 virt + NS16550 |
| [sifive_u](sifive_u/) | riscv | sifive-u54 | 需 `CORES>=2` |
| [qemu_virt_a64](qemu_virt_a64/) | arm64 | cortex-a53（≈A55）, cortex-a72（≈A75） | 单核；需 aarch64 工具链 |

详见 [docs/PORTING.md](../docs/PORTING.md)。
