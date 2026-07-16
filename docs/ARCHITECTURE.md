# CG-RTOS 主流程与架构详解

本文配合源码中的 **Doxygen** 注释（`kernel/cgrtos.h`、各 `.c` / `startup.S`），用流程图说明系统主路径。

生成 API 文档 / SDK：

```bash
./scripts/cgrtos.sh sdk    # docs/doxygen + 打包 sdk/
# 或仅 HTML：
./scripts/cgrtos.sh docs
```
---

## 1. 总体结构

```mermaid
flowchart TB
  subgraph Boot["启动 Boot"]
    S["_start / _start_secondary"]
    M["main / secondary_main"]
    I["cgrtos_init"]
    K["cgrtos_smp_kick_secondaries"]
    ST["cgrtos_start / cgrtos_start_secondary"]
    S --> M --> I --> K --> ST
  end

  subgraph Run["运行时 Runtime"]
    T["trap_vector"]
    TH["cgrtos_tick_handler"]
    SW["cgrtos_sched_switch_from_trap"]
    LB["cgrtos_sched_load_balance"]
    T --> TH --> LB
    T --> SW
  end

  subgraph App["应用 / IPC"]
    TC["cgrtos_task_*"]
    IPC["sem / mutex / queue / event / stream / msg / qset"]
    FS["RAM FS"]
    TM["soft timer + Tmr Svc"]
    HEAP["TLSF malloc/free"]
  end

  ST --> Run
  App --> SW
  TH --> TM
```

| 层级 | 路径 | 职责 |
|------|------|------|
| 引导 | `startup.S` | BSS、双核同步、trap、首次 `mret` |
| 内核核 | `kernel/cgrtos.c` | 初始化、临界区、统计、SMP kick |
| 调度 | `kernel/scheduler.c` | 就绪队列、tick、切换、均衡 |
| 任务 | `kernel/task.c` | 创建/删除/延时/通知/idle |
| IPC | `kernel/ipc.c` | 信号量/互斥/队列/事件 |
| 流缓冲 | `kernel/stream_buffer.c` | StreamBuffer / MessageBuffer |
| QueueSet | `kernel/queue_set.c` | 多对象 select |
| RAM FS | `kernel/fs.c` | `/` 纯内存文件/目录 |
| 定时 | `kernel/timer.c` | 时间轮 + daemon |
| 堆 | `kernel/mem.c` | TLSF |
| 平台 | `arch/riscv/*` | CLINT/UART/PLIC/IPI |
| 公开头 | `kernel/cgrtos.h` | 全部宏/类型/API（Doxygen） |
| SDK 包 | `./scripts/cgrtos.sh sdk` | `sdk/include` + `sdk/docs/api` |

---

## 2. 双核 Boot 流程

```mermaid
sequenceDiagram
  participant H0 as Hart0
  participant H1 as Hart1
  participant DRAM as g_boot_sync

  Note over H0,H1: QEMU 需 -device loader,addr=0xA0000000,cpu-num=1

  H0->>H0: mtvec, mhartid==0
  H1->>H1: mtvec, mhartid==1 → _start_secondary
  H0->>H0: I-cache only, 清 _sbss.._ebss
  H0->>DRAM: store 0xCAFE5A5A
  H0->>H0: main → cgrtos_init → kick
  H1->>H1: stage=1..2, 设 gp/sp
  H1->>H1: 忙等延时 (勿 WFI)
  H1->>DRAM: lwu 轮询 magic
  DRAM-->>H1: match → stage=4
  H1->>H1: secondary_main
  H0->>H0: 创建任务, cgrtos_start, g_sched_run=1
  H1->>H1: 等 g_sched_run, MSIE, g_secondary_online=1
  H1->>H1: start_first_task(idle1)
  H0->>H0: start_first_task(first)
```

要点：

1. **`lwu` 而非 `lw`**：magic `0xCAFE5A5A` 最高位为 1，`lw` 符号扩展后与 `li` 结果不相等。
2. **`.sbss` 必须算进 BSS 清零**（见 `cgrtos.lds`）。
3. **QEMU 上 hart1 禁止长期 WFI**，否则共享 `mtime` 可能停。
4. **`g_secondary_online` 仅在进入调度后置位**，避免 LB 把任务迁到尚未调度的核。

---

## 3. 调度与上下文切换

```mermaid
flowchart LR
  subgraph ISR["中断 / 陷阱"]
    TV["trap_vector 保存栈帧"]
    CS["清 MSIP / 重装 mtimecmp"]
    YP{"g_yield_pending?"}
    SW["cgrtos_sched_switch_from_trap"]
    TV --> CS --> YP
    YP -->|是| SW
    YP -->|否| Restore["trap_restore + mret"]
    SW --> Restore
  end

  subgraph Tick["Hart0 Tick"]
    TH["cgrtos_tick_handler"]
    DEL["delayed / EDF wheel"]
    TMR["cgrtos_timer_process_tick"]
    LB["load_balance"]
    TH --> DEL --> TMR --> LB
  end
```

就绪选择（简化）：

```mermaid
flowchart TD
  P["sched_pick_next(cpu)"]
  P --> E{"EDF 有到期?"}
  E -->|是| EDF["取最近 deadline"]
  E -->|否| PR{"优先级 bitmap 非空?"}
  PR -->|是| HI["最高 prio 队头"]
  PR -->|否| CFS{"CFS 链非空?"}
  CFS -->|是| VR["最小 vruntime"]
  CFS -->|否| IDLE["g_idle[cpu]"]
```

策略差异：

| 策略 | 行为 |
|------|------|
| `SCHED_PRIORITY` | 同等优先级粘滞（tick 不强制轮转，除非 `force_yield`） |
| `SCHED_RR` | 时间片轮转 |
| `SCHED_CFS` | `vruntime` 排序 |
| `SCHED_EDF` | deadline 排序 + release wheel |
| `SCHED_HYBRID` | `prio >= CONFIG_RT_PRIO_THRESHOLD` → Priority，否则 CFS |

---

## 4. SMP 加权负载均衡（Push）

```mermaid
flowchart TD
  A["cgrtos_sched_load_balance @ hart0 tick"]
  A --> B{"g_secondary_online?"}
  B -->|否| Z[返回]
  B -->|是| C["算 L0 L1 加权负载"]
  C --> D{"|Lbusy-Lidle| > HYST?"}
  D -->|否| Z
  D -->|是| E["挑受害者: 低 weight / 高 vruntime"]
  E --> F["不可迁: EDF / 硬亲和 / 高 prio 定时服务"]
  F --> G["remove_ready → run_cpu=dst → add_ready"]
  G --> H["MSIP IPI 唤醒 dst"]
  H --> D
```

Idle Steal（`CONFIG_SMP_IDLE_STEAL`，默认 0）在空闲循环反向拉取。

跨核唤醒：`cgrtos_sched_unblock` / `cgrtos_task_create` / `set_affinity` 在目标核 ≠ 本核时发 IPI。

---

## 5. IPC / 阻塞 / 唤醒

```mermaid
sequenceDiagram
  participant T as Task A
  participant S as Sem/Queue/...
  participant Sch as Scheduler
  participant B as Task B

  T->>S: take / recv (count==0)
  S->>Sch: sched_block + wait_q
  T->>Sch: yield → BLOCKED
  B->>S: give / send
  S->>Sch: unblock(A) + maybe IPI
  Sch->>T: READY on target core
  Note over T: 下次 switch 运行, wake_ok=1
```

临界区：`cgrtos_enter_critical` = 关本核 IRQ + 全局 `g_klock`（可嵌套）。  
`g_ready_lock` 侧：关本地 IRQ 后再抢锁，避免 ISR 重入死锁。

补充对象（与 Queue 相同阻塞模型）：

- **StreamBuffer / MessageBuffer**（`kernel/stream_buffer.c`）：字节流与整消息（长度前缀）
- **QueueSet**（`kernel/queue_set.c`）：对 Queue/Sem/Stream 做 `select`
- **抢占**：tick / 唤醒路径上更高优先级任务可抢占当前任务（`SCHED_PRIORITY` 等）
- **RAM FS**（`kernel/fs.c`）：`/` 挂载，`open/read/write/mkdir/unlink/readdir`

---

## 6. 软定时器

```mermaid
flowchart LR
  Tick["hart0 tick"] --> Wheel["timer wheel 走格"]
  Wheel -->|到期| Q["queue 投递 TIMER_CMD_EXPIRE"]
  Q --> Daemon["Tmr Svc 任务"]
  Daemon --> CB["用户 callback"]
```

回调**不在 ISR 里执行**，在 daemon 任务上下文执行。

---

## 7. TLSF 堆（`mem.c`）

```mermaid
flowchart TD
  M["cgrtos_malloc"] --> Map["size → FL/SL"]
  Map --> Find["找合适 free 块"]
  Find --> Split["可选拆分"]
  Split --> User["返回 user ptr"]
  F["cgrtos_free"] --> Merge["与相邻 free 合并"]
  Merge --> Note["必须修正后继 prev_phys"]
  Note --> Insert["插回 freelist"]
```

---

## 8. 陷阱向量类别

| mcause | 处理 |
|--------|------|
| 中断 3 MSIP | `riscv_handle_ipi` → `g_yield_pending` |
| 中断 7 MTIP | `riscv_handle_timer` → tick（hart0 推进 `g_ticks`） |
| 中断 11 MEIP | PLIC claim / complete |
| 异常 11 ecall | 自愿 yield（`g_yield_pending`） |
| 其它异常 | 打印后 `_deadloop` |

---

## 9. 相关源码入口

| 主题 | 符号 / 文件 |
|------|-------------|
| Boot | `_start`, `_start_secondary`, `g_boot_sync` |
| 启动 API | `cgrtos_init`, `cgrtos_start`, `cgrtos_start_secondary` |
| 调度 | `cgrtos_tick_handler`, `cgrtos_sched_switch_from_trap` |
| 均衡 | `cgrtos_sched_load_balance`, `cgrtos_sched_idle_steal` |
| 公开头文件 | `kernel/cgrtos.h`（全部宏/类型/API 的 Doxygen） |

完整用法、SMP 算法、GDB 实战：见 `docs/USER_GUIDE.md`。
