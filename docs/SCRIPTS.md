# CG-RTOS 编译 / 运行脚本说明

入口脚本：`scripts/cgrtos.sh`。另有薄封装：

| 脚本 | 作用 |
|------|------|
| `./scripts/build.sh …` | **只编译**（等价 `cgrtos.sh build`） |
| `./scripts/run.sh …` | **只运行**已有镜像（等价 `cgrtos.sh run`） |
| `./scripts/cgrtos.sh …` | 完整命令集（build / run / test / clangd / docs） |
| `./scripts/port-check.sh` | 多架构 test+stress 门禁（可选 smoke） |
| `./scripts/run_qemu.sh` | 已废弃，转发到 `run` |

**原则：编译与运行分离。** `build` 永不启动 QEMU；`run` 永不调用 `make`。  
对象在 `build/<BOARD>/`；`cgrtos.elf` / `cgrtos.bin` 仍在仓库根。ARM64 用 `BOARD_QEMU_LOAD=kernel`（`-kernel cgrtos.elf`），不必依赖 `.bin` 作为启动镜像。

换板请带 `--board` / `--cpu` / `--cores`；ARM64 需 `PATH`/`LD_LIBRARY_PATH` 指向 `cgrtos-tools`（见 `docs/PORTING.md`）。

---

## 1. 推荐工作流

```bash
# ① 编译（同时刷新 clangd 的 compile_commands.json）
./scripts/build.sh --app test --cores 2

# ② 用 Cursor / clangd 浏览、跳转（打开仓库根目录即可）

# ③ 运行已有镜像（不重新编译）
./scripts/run.sh --app test --cores 2 --timeout 180

# ④ 改代码后：再 build → 再 run
./scripts/build.sh --app test --cores 2
./scripts/run.sh --app test --cores 2
```

一键「编完就跑」仍可用：

```bash
./scripts/cgrtos.sh test --cores 2      # = build + run
./scripts/cgrtos.sh stress --cores 2
./scripts/cgrtos.sh demo
```

跳过编译（二进制已存在）：

```bash
./scripts/cgrtos.sh test --no-build --cores 2
```

---

## 2. 命令一览

### 2.1 只编译

```bash
./scripts/cgrtos.sh build [选项]
./scripts/build.sh [选项]
```

| 行为 | 说明 |
|------|------|
| 产物 | `cgrtos.elf`、`cgrtos.bin`、`cgrtos.map` |
| clangd | 默认用 [bear](https://github.com/rizsotto/Bear) 生成/刷新根目录 `compile_commands.json`（`make -B` 全量编译一次以保证数据库完整） |
| `--no-compdb` | 跳过 bear，仅增量 `make all`（更快） |
| `--clean` | 先 `make clean` |
| 不启动 QEMU | 是 |

仅刷新编译数据库（不关心产物是否最新时也可用）：

```bash
./scripts/cgrtos.sh compdb --app test --cores 2
```

### 2.2 只运行

```bash
./scripts/cgrtos.sh run [选项]
./scripts/run.sh [选项]
```

| 行为 | 说明 |
|------|------|
| 前提 | 已有 `cgrtos.bin`（且 `--gdb` 时需要 `cgrtos.elf`） |
| 缺失时 | 打印错误并提示先执行 `build`，**不会**自动 make |
| QEMU | Nuclei `nuclei_evalsoc`，`-smp` = `--cores` |
| 超时 | `--timeout SEC`（默认 120） |
| 结果 | 根据串口输出打印 `RESULT: …`，非 0 表示失败/超时 |

### 2.3 便捷命令（默认先 build 再 run）

| 命令 | APP | 默认超时 | 成功标记 |
|------|-----|----------|----------|
| `test` | test | 120s | `=== TEST_SUITE_PASSED ===` |
| `demo` | demo | 120s | `[T1] LED OFF` |
| `bench` | bench | 30s | `=== BENCH_DONE ===` |
| `stress` | stress | 60s | `=== STRESS_PASSED ===` |
| `cli` | cli | 交互 | 手动 `Ctrl-A` `X` 退出 |
| `gdb` | 当前 `--app` | — | QEMU `-S` + 另窗 GDB TUI |

### 2.4 文档 / SDK

```bash
./scripts/cgrtos.sh docs    # Doxygen → docs/doxygen/html/
./scripts/cgrtos.sh sdk     # docs + 打包 sdk/include + sdk/docs/api
```

---

## 3. 选项与环境变量

### 选项

| 选项 | 含义 |
|------|------|
| `--app NAME` | `demo` \| `test` \| `bench` \| `stress` \| `cli` |
| `--cores N` | `1` \| `2` \| `4`（默认 **2**） |
| `--board NAME` | BSP 目录名，对应 `boards/<NAME>/`（默认 `nuclei_evalsoc`） |
| `--profile NAME` | 如 `minimal` → `-DCGRTOS_CONFIG_MINIMAL=1` |
| `--clean` | build 前 clean |
| `--no-compdb` | build 时不生成 `compile_commands.json` |
| `--no-build` | 便捷命令跳过 make，直接用现有 bin |
| `--gdb` / `-g` | QEMU gdbstub + 第二窗口 GDB |
| `--timeout SEC` | QEMU 墙钟超时 |
| `--port N` | GDB 端口（默认 1234） |

快捷：`--demo` / `--test` / `--bench` / `--stress` / `--cli` 等价于设置 `--app`。

### 环境变量

| 变量 | 默认 | 用途 |
|------|------|------|
| `SYSROOT` | Nuclei 交叉工具链根 | `PATH` + gcc |
| `QEMU` | Nuclei qemu-system-riscv64 | 仿真 |
| `GDB` | `$SYSROOT/.../gdb` | 调试 |
| `CORES` / `BOARD` / `PROFILE` / `APP` | 见上 | 可被 CLI 覆盖 |
| `CGRTOS_GDB_TERM` | `auto` | `wt` \| `tmux` \| `xterm` \| `here` |
| `TIMEOUT_SEC` | `120` | 可被 `--timeout` 覆盖 |

Makefile 捷径（内部转调脚本）：

```bash
make APP=test CORES=2          # 只编
make test                      # cgrtos.sh test
make run APP=test              # cgrtos.sh run --no-build（需已编过）
make CORES=1 PROFILE=minimal APP=demo
```

---

## 4. clangd / Cursor 代码浏览

1. 安装扩展：**clangd**（建议禁用微软 C/C++ 扩展的 IntelliSense，避免冲突）。
2. 在仓库根执行一次：

   ```bash
   ./scripts/build.sh --app test --cores 2
   # 或
   ./scripts/cgrtos.sh compdb --app test --cores 2
   ```

3. 确认根目录存在 `compile_commands.json`，以及 `.clangd`（已指向 `CompilationDatabase: .`）。
4. **Reload Window**（命令面板）后再打开 `kernel/*.c`，应能正确解析 `#include "cgrtos.h"`、跳转到定义、看宏展开。
5. 切换 `--cores` / `--profile` / `APP` 后请重新 `build` 或 `compdb`，否则 clangd 里的 `CONFIG_NUM_CORES` 等宏会过期。

无 `bear` 时脚本会回退到解析 `make -n` 生成简易数据库（功能较弱）；本环境通常已有 `/usr/bin/bear`。

---

## 5. 常用示例

```bash
# 全功能测试（双核）
./scripts/build.sh --app test --cores 2
./scripts/run.sh  --app test --cores 2 --timeout 180

# 单核 / 四核
./scripts/cgrtos.sh test --cores 1
./scripts/cgrtos.sh test --cores 4

# 最小裁剪镜像（仅编译验证）
./scripts/build.sh --app demo --cores 1 --profile minimal --clean

# 压力 + 微基准
./scripts/cgrtos.sh stress --cores 2
./scripts/cgrtos.sh bench  --cores 2

# 交互 CLI
./scripts/cgrtos.sh cli --cores 2

# 调试：本窗 UART，另窗 GDB TUI
./scripts/cgrtos.sh gdb --app test --cores 2
./scripts/run.sh --app test --cores 2 --gdb   # 不重编
```

退出 QEMU：`Ctrl-A` 然后 `X`。

---

## 6. 故障排查

| 现象 | 处理 |
|------|------|
| `cgrtos.bin missing` | 先 `./scripts/build.sh …` |
| clangd 找不到头文件 | 重新 `compdb` / `build`，Reload Window |
| 改了 `CORES=` 行为怪 | **全量重编**（`build --clean`）；arch 目标也需更新 |
| `RESULT: TIMEOUT` | 加大 `--timeout`；检查是否有残留 `qemu-system-riscv64` |
| 工具链 / QEMU 找不到 | 设置 `SYSROOT`、`QEMU` 环境变量 |
| `PROFILE=minimal` 链不过 | 确认 `APP` 未依赖被裁掉的 CLI/FS 等 |

更完整的产品说明见 [USER_GUIDE.md](USER_GUIDE.md)；调试与性能模块见 [MODULE_DBG_PERF.md](MODULE_DBG_PERF.md)。
