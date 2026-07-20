# CG-RTOS 使用指南

Freestanding RISC-V64 **双核 SMP** 实时内核（Nuclei UX900 / `nuclei_evalsoc`）。

| 文档 | 内容 |
|------|------|
| 本文 | 编译、运行、CLI、常用 API、GDB |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 启动 / 调度 / 陷阱流程图 |
| `./scripts/cgrtos.sh sdk` | 公开头 + API HTML（`sdk/`） |

---

## 1. 五分钟上手

```bash
# 工具链 / QEMU 可用环境变量覆盖（脚本已带默认路径）
export SYSROOT=...   # 可选
export QEMU=...      # 可选

./scripts/cgrtos.sh test    # 完整功能套件 → RESULT: TEST_SUITE_PASSED
./scripts/cgrtos.sh cli     # 交互：list / run <case>
./scripts/cgrtos.sh sdk     # 生成 sdk/include/cgrtos.h + sdk/docs/api/
```

退出 QEMU：`Ctrl-A` 然后 `X`。

---

## 2. 脚本一览（唯一推荐入口）

```bash
./scripts/cgrtos.sh <命令> [选项]
```

| 命令 | 作用 |
|------|------|
| `test` | 编译并跑全部功能 case |
| `cli` | UART 交互 CLI，按名跑 case |
| `stress` | 多任务压力（**不**包含在 `run all`） |
| `demo` / `bench` | 演示 / 微基准 |
| `build --app X` | 只编译 |
| `run --app X [--no-build]` | 编译（可选）并启动 QEMU |
| `gdb` / `--gdb` | 本窗 QEMU，另窗 GDB TUI（C 源码） |
| `docs` | 仅 Doxygen → `docs/doxygen/html/` |
| `sdk` | Doxygen + 打包 `sdk/` |
| `help` | 内置帮助 |

常用选项：`--app demo|test|bench|stress|cli`、`--gdb`/`-g`、`--no-build`、`--timeout SEC`、`--port N`。

Makefile 捷径：`make test` / `make cli` / `make sdk` 等，内部都转调 `cgrtos.sh`。

旧入口 `./scripts/run_qemu.sh` 仍可用，已标记废弃，请改用 `cgrtos.sh`。

环境变量：`SYSROOT`、`QEMU`、`GDB`、`CGRTOS_GDB_TERM=auto|wt|tmux|xterm|here`。

---

## 3. CLI 与测试 case

```bash
./scripts/cgrtos.sh cli
# cgrtos> help
# cgrtos> list
# cgrtos> run mem
# cgrtos> run streambuf
# cgrtos> run all          # 全部功能 case（不含 stress）
# cgrtos> run stress       # 单独压力
# cgrtos> stats / heap / cores
```

| Case | 覆盖 |
|------|------|
| `delay` | tick / `delay_ms` / `delay_us` / `delay_until` |
| `mem` | TLSF malloc / calloc / free |
| `sem` / `mutex` / `queue` / `event` | 经典 IPC（mutex 含递归） |
| `safety` | 递归锁 / 删任务释锁 / 栈金丝雀 / `stats_get` |
| `streambuf` / `msgbuf` / `qset` | 流 / 消息缓冲 / QueueSet |
| `fs` | RAM FS open/read/write/mkdir/… |
| `preempt` | 高优先级抢占忙等低优先级 |
| `notify` / `timer` / `task` | 通知、软定时器、生命周期 |
| `sched` / `smp` | 多策略调度、亲和与负载均衡 |
| `hooks` / `critical` | hooks、静态 IPC、临界区 |

调试 CLI：`./scripts/cgrtos.sh cli --gdb`。

---

## 4. 应用侧常用 API（摘要）

唯一公开头：`#include "cgrtos.h"`（源码在 `kernel/cgrtos.h`；SDK 包在 `sdk/include/`）。

### 4.1 延时

| API | 语义 |
|-----|------|
| `cgrtos_delay(0)` | 仅 yield |
| `cgrtos_delay(n)` | 相对 tick 阻塞 |
| `cgrtos_delay_ms` / `cgrtos_delay_us` | mtime 绝对截止 + tick 粗阻塞 |
| `cgrtos_delay_until` | FreeRTOS 周期语义（错过则不拉长） |

### 4.2 StreamBuffer / MessageBuffer

```c
cgrtos_stream_buffer_t *sb = cgrtos_stream_buffer_create(256, 1);
cgrtos_stream_buffer_send(sb, data, len, portMAX_DELAY);
cgrtos_stream_buffer_recv(sb, buf, len, portMAX_DELAY);

cgrtos_message_buffer_t *mb = cgrtos_message_buffer_create(256);
cgrtos_message_buffer_send(mb, msg, n, portMAX_DELAY);  /* 整消息 */
cgrtos_message_buffer_recv(mb, out, sizeof out, portMAX_DELAY);
```

Message 在环内布局：`[u16 LE 长度][payload]`。

### 4.3 QueueSet

```c
cgrtos_queue_set_t *set = cgrtos_queue_set_create(0);
cgrtos_queue_set_add_queue(set, q);
cgrtos_queue_set_add_sem(set, sem);
void *ready = cgrtos_queue_set_select(set, portMAX_DELAY);
/* 再对 ready 指针做 queue_recv / sem_take / stream_recv */
```

### 4.4 RAM 文件系统

根为 `/`；路径须绝对路径。

```c
cgrtos_fs_mkdir("/tmp");
int fd = cgrtos_fs_open("/tmp/a.txt", CGRTOS_O_CREAT | CGRTOS_O_RDWR);
cgrtos_fs_write(fd, "hi", 2);
cgrtos_fs_lseek(fd, 0, 0);
cgrtos_fs_read(fd, buf, 2);
cgrtos_fs_close(fd);
cgrtos_fs_unlink("/tmp/a.txt");
cgrtos_fs_rmdir("/tmp");
```

池上限见头文件：`CGRTOS_FS_MAX_INODES` / `CGRTOS_FS_MAX_FD` / `CGRTOS_FS_MAX_FILE_BYTES`。

### 4.5 安全与可观测性

| API | 作用 |
|-----|------|
| 递归 `mutex_lock`/`unlock` | 同任务可重入；`get_recursive_count` / `get_holder` |
| `cgrtos_task_delete` | 禁删 idle；释放持有互斥量；延迟回收 TCB |
| `CGRTOS_ASSERT` / `configASSERT` | 失败计数 + 钩子 + halt |
| `cgrtos_stats_get` / `stats_dump` | 切换/创建删除/栈溢/断言/堆水位 |
| `cgrtos_task_check_stack` | 金丝雀检测；切换与 tick 抽检 |

---

## 5. 环境依赖

| 组件 | 默认路径（可用环境变量覆盖） |
|------|------------------------------|
| 交叉工具链 | `SYSROOT` → Nuclei ext-toolchain |
| QEMU | `QEMU` → `qemu-system-riscv64` |
| GDB | `$SYSROOT/bin/riscv64-unknown-linux-gnu-gdb` |
| Doxygen | 系统 `doxygen`（`sdk`/`docs` 需要） |

QEMU **必须**带 hart1 loader，否则 SMP 假绿：

```text
-device loader,addr=0xA0000000,cpu-num=1
```

`cgrtos.sh` 已默认带上。

---

## 6. GDB 双窗口调试

```bash
./scripts/cgrtos.sh test --gdb
./scripts/cgrtos.sh cli --gdb
./scripts/cgrtos.sh gdb --app stress
```

- **本窗**：QEMU UART  
- **另窗**：`gdb -tui` + `scripts/gdbinit`，停在 `main`，`layout src` 看 C 源码  

窗口选择：Windows Terminal（WSL）→ tmux → xterm → 同 tty。可用 `CGRTOS_GDB_TERM` 强制。

常用：`cgrtos-cores`、`cgrtos-tasks`、`break cgrtos_sched_load_balance`。

手动两窗与 VS Code 配置见下文附录。

---

## 7. SMP 负载均衡（要点）

- **Push**：hart0 tick 调 `cgrtos_sched_load_balance()`，按加权负载迁任务并发 IPI  
- **Steal**：`CONFIG_SMP_IDLE_STEAL` 默认 **关**（QEMU 上 Push 已够稳）  
- **初始放置**：`CONFIG_SMP_INITIAL_PLACE=1` 时新建任务落在更轻核  
- EDF / 硬亲和任务不迁移  

细节与 Boot 陷阱（`lwu`、`.sbss`、勿对 QEMU 长期 WFI）：见 [ARCHITECTURE.md](ARCHITECTURE.md)。

---

## 7.1 中断安全 API（FromISR）与优先级分组

**FromISR 约定（FreeRTOS 风格）：**

- 第二个/额外参数 `BaseType_t *woken`：非空则唤醒更高优先级任务时置 `pdTRUE`，由 ISR 末尾 `portYIELD_FROM_ISR(woken)` 请求调度；传 `NULL` 则自动 `cgrtos_sched_yield_from_isr()`。
- 已提供：`sem_give/take_from_isr`、`queue_send/recv_from_isr`、`event_set/clear_from_isr`、`stream_*_from_isr`、`message_buffer_*_from_isr`、`task_notify_from_isr`、`timer_{start,stop,reset,change_period}_from_isr`。

**优先级分组：**

| 宏 / API | 含义 |
|----------|------|
| `CONFIG_IRQ_SYSCALL_MAX_PRIO` | 允许调用 FromISR 的最高优先级（默认 3） |
| `cgrtos_irq_set/get_syscall_max_priority` | 运行时调整上界 |
| `cgrtos_irq_configure` / `cgrtos_plic_set_priority` | 配置源优先级 |
| `cgrtos_enter/exit_critical_from_isr` | 抬高 PLIC threshold，屏蔽 ≤ syscall_max 的中断 |

**快速响应：** `riscv_handle_external` 在分发前抬高 PLIC threshold；当 `CONFIG_IRQ_NESTING=1` 时关闭 MTIE/MSIE 后短暂打开 `MIE`，仅允许更高优先级 **外部** 中断嵌套（不得调用 FromISR）。QEMU 不稳时可置 `CONFIG_IRQ_NESTING=0`。

```c
BaseType_t woken = pdFALSE;
cgrtos_sem_give_from_isr(sem, &woken);
portYIELD_FROM_ISR(woken);
```

专项测试：`./scripts/cgrtos.sh cli` → `run isr` / `run irq`；全量 `./scripts/cgrtos.sh test`。

---

## 8. SDK 与目录

```bash
./scripts/cgrtos.sh sdk
# sdk/include/cgrtos.h
# sdk/docs/api/index.html
# sdk/README.md
```

```
kernel/          调度 / 任务 / IPC / Stream / QueueSet / 定时器 / TLSF / RAM FS
arch/riscv/      CLINT / UART / PLIC / IPI
tests/           test / cli / bench / stress
scripts/cgrtos.sh  一键入口
scripts/gdbinit    GDB 辅助
docs/              USER_GUIDE / ARCHITECTURE / doxygen
sdk/               由 sdk 命令生成（可 rm；勿手改）
```

---

## 9. 验收清单

- [ ] `./scripts/cgrtos.sh test` → `TEST_SUITE_PASSED`（`secondary=1`）
- [ ] `./scripts/cgrtos.sh cli` → `run streambuf|msgbuf|qset|fs|preempt` 全 PASS
- [ ] `./scripts/cgrtos.sh sdk` → 可打开 `sdk/docs/api/index.html`
- [ ] `./scripts/cgrtos.sh test --gdb` 停在 `main`，`cgrtos-cores` 可用

---

## 附录 A — 手动 GDB

**终端 A：**

```bash
make clean all APP=test
qemu-system-riscv64 \
  -M nuclei_evalsoc,download=ddr -smp 2 -m 512M \
  -cpu nuclei-ux900fd,ext=svpbmt_zicbom_sstc_sscofpmf_zba_zbb_zbc_zbs_zicond \
  -nographic -serial mon:stdio \
  -bios cgrtos.bin \
  -device loader,addr=0xA0000000,cpu-num=1 \
  -gdb tcp::1234 -S
```

**终端 B：**

```bash
riscv64-unknown-linux-gnu-gdb -tui -ex 'target remote :1234' -x scripts/gdbinit cgrtos.elf
```

## 附录 B — Cursor / VS Code Attach 示例

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "CG-RTOS QEMU GDB",
      "type": "gdb",
      "request": "attach",
      "executable": "${workspaceFolder}/cgrtos.elf",
      "target": ":1234",
      "remote": true,
      "cwd": "${workspaceFolder}",
      "gdbpath": "${env:SYSROOT}/bin/riscv64-unknown-linux-gnu-gdb",
      "autorun": ["set architecture riscv:rv64"]
    }
  ]
}
```

先起 QEMU stub（`cgrtos.sh … --gdb` 或手动 `-S`），再 Attach。
