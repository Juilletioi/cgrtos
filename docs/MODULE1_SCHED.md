# 模块1：调度器增强 — 设计说明

版本：CG-RTOS **4.9.0**  
范围：核心调度层（`kernel/scheduler.c` / `task.c` / `ipc.c` 互斥协议）  
约束：宏可裁剪、热路径精简、API 参数校验、应用不直访内核全局表

---

## 1. 设计思路

在现有 **多策略 SMP 调度**（FP/RR/CFS/MC-EDF/Hybrid）之上补齐实时与可观测性缺口：

| 能力 | 方案 | 宏开关（默认） |
|------|------|----------------|
| EDF / MC-EDF | 全局 deadline 链 + 释放轮；可整模块裁剪 | `CONFIG_USE_EDF=1` |
| FP↔EDF 共存 | 任务级 `policy`；关闭 EDF 后创建 `SCHED_EDF` 失败 | 同上 |
| 同优先级 RR | `SCHED_RR` + `CONFIG_TIME_SLICE_TICKS` | 已有 |
| 抢占阈值 | 每任务 `preempt_thresh`：仅 `prio > thresh` 可抢占 | `CONFIG_USE_PREEMPT_THRESH=1` |
| DPCP（截止期限/优先级天花板） | 互斥量天花板；EDF 抬 deadline、FP 抬 prio | `CONFIG_USE_DPCP=1` |
| FP 优先级反转 | 既有优先级继承 PI | `mutex->inherit` |
| 状态闭环 | 增加 `TASK_TERMINATED`；入口 trampoline → `task_exit` | 常开 |
| 调度统计 | 就绪→运行延迟、最大延迟、CPU 占用率 | `CONFIG_SCHED_STATS=1` |

**分层**：调度层只操作就绪/延迟/EDF 结构（`g_ready_lock`）；IPC 在 `g_klock` 临界区内调用调度入队/出队 API，不直接改位图。

**热路径**：`switch_from_trap` 仍为 `irq_save → ready_lock → pick → requeue → unlock`；阈值判断为常数次比较，不引入堆遍历。

---

## 2. 关键 API

```c
/* 抢占阈值：thresh >= prio；越大越难被抢占 */
int cgrtos_task_set_preempt_threshold(task_id_t id, uint8_t thresh);
uint8_t cgrtos_task_get_preempt_threshold(task_id_t id);

/* 任务正常退出（入口返回或显式调用）→ TERMINATED → DELETED */
void cgrtos_task_exit(void);

/* DPCP 互斥：ceiling_prio 用于 FP；ceiling_rel 为相对 deadline 天花板（EDF） */
cgrtos_mutex_t *cgrtos_mutex_create_dpcp(uint8_t ceiling_prio, tick_t ceiling_rel);
int cgrtos_mutex_set_ceiling(cgrtos_mutex_t *m, uint8_t ceiling_prio, tick_t ceiling_rel);

/* 调度统计 */
typedef struct {
    tick_t   max_latency;      /* 就绪→首次运行 最大 tick */
    tick_t   last_latency;
    uint64_t latency_sum;
    uint32_t latency_samples;
    tick_t   exec_ticks;
    uint32_t cpu_util_permille; /* exec/uptime * 1000 */
} cgrtos_task_sched_stats_t;

int cgrtos_task_get_sched_stats(task_id_t id, cgrtos_task_sched_stats_t *out);
void cgrtos_sched_stats_get(tick_t *max_latency_global, uint32_t *samples);
void cgrtos_sched_stats_reset(void);
```

既有：`cgrtos_task_set_priority`、`cgrtos_delay*`、事件/IPC 超时阻塞、`SCHED_RR`/`SCHED_EDF`。

---

## 3. 协议与抢占规则

### 3.1 抢占阈值（Preemption Threshold）

- 默认 `preempt_thresh = prio`（与经典抢占一致）。
- 运行中任务仅当候选 `next->prio > cur->preempt_thresh` 时放弃粘性（FP/Hybrid-RT）。
- EDF 候选：仍按 `CONFIG_MCEDF_PRIO_SLACK_TICKS` 紧迫窗口；阈值不改变 MC-EDF 全局分配，仅抑制「不紧迫 EDF」打断高阈值 FP。

### 3.2 DPCP（本实现语义）

即时天花板（ICPP 风格，适配单资源临界区）：

1. 锁上配置 `ceiling_prio` / `ceiling_rel`（相对截止期限）。
2. **加锁成功**：保存 `owner` 的 `prio`/`deadline`；  
   - FP：`prio = max(prio, ceiling_prio)`；  
   - EDF：`deadline = min(deadline, now + ceiling_rel)`（`ceiling_rel!=0`）。  
3. **解锁**：恢复保存值；若仍有等待者，按协议抬升新 owner。  
4. 与 PI 互斥：`proto=DPCP` 时不做 waiter→owner 的动态 PI 提升（天花板已覆盖）。

### 3.3 状态机

```
CREATE → READY ⇄ RUNNING ⇄ BLOCKED
              ↘ SUSPENDED
RUNNING/READY/BLOCKED → TERMINATED → DELETED（槽回收）
```

入口经 `task_bootstrap`：用户函数返回后必进 `cgrtos_task_exit()`，避免「函数返回后跑飞」。

---

## 4. 边界与异常

| 场景 | 行为 |
|------|------|
| `set_preempt_threshold` 且 `thresh < prio` | `pdFAIL` |
| `CONFIG_USE_EDF=0` 创建 EDF | `(task_id_t)-1` |
| DPCP `ceiling_prio > MAX` | `pdFAIL` |
| ISR 中 `task_exit` / 阻塞 API | 既有路径拒绝 / assert 钩子（后续模块4加强） |
| 统计未开启 | API 返回 `pdFAIL` 或填 0 |

---

## 5. 竞态与临界区

| 数据 | 保护 |
|------|------|
| 就绪位图 / CFS / EDF 全局链 | `irq_save` + `g_ready_lock` |
| 延迟链 / IPC wait_q / mutex owner | `cgrtos_enter_critical`（`g_klock`） |
| 统计累加 | 在持 `g_ready_lock` 的 switch 路径更新，或关本核 IRQ 写本任务字段 |
| DPCP 改 prio/deadline | 在 `g_klock` 内；READY 时先 `remove_ready` 再改再 `add_ready` |

**风险点 A（讲解）**：持 `g_klock` 时同步 `sched_mcedf_kick_all`（IPI）可与对端 `enter_critical` 形成死锁面。实现改为 `sched_edf_kick_request`：临界区/ISR 内仅置 `g_edf_kick_pending`，在 `exit_critical` 最外层调用 `cgrtos_sched_edf_kick_flush`；ISR flush 只标 `yield_pending` 不发 IPI。

**风险点 B**：自删除任务在 `switch_from_trap` 中 `TASK_DELETED` 延迟 reclaim，避免 use-after-free；`TERMINATED` 走同一回收路径。

**风险点 C**：多核上 owner 被抬升优先级时，若 RUNNING 在它核，需 IPI 触发重评估，否则阈值/天花板可能短时间不生效。

**风险点 D（讲解）**：测试里用 `yield` 忙等 tick 会在「EDF 占满核」时饿死测试任务导致 TIMEOUT；`wait_ticks` 必须用 `cgrtos_delay`。EDF/延迟链入队前先 `remove`，防止双链成环后 tick 路径关中断死循环。

---

## 6. FP vs EDF 路径开销（优化方向）

| 路径 | 复杂度 | 说明 |
|------|--------|------|
| FP pick | O(1) | 位图 `__builtin_clz` + 队头 |
| EDF pick | O(n) 分配扫描 | 全局链按 deadline 填 m 席；n=就绪 EDF 数 |
| EDF insert | O(n) | 有序链表；模块5可换最小堆 |

本模块保持链表 MC-EDF；热路径避免在 switch 内做释放轮扫描（仍仅 hart0 tick）。

---

## 7. 测试要点

- `case_sched`：CFS 公平 + EDF hits/miss + Hybrid。  
- `case_sched_m1`（`sched_m1`）：PT / exit / stats / DPCP。  
- 回归：`./scripts/cgrtos.sh test --cores {1,2,4}` + `stress --cores 2`。

### 已知竞态与防护

| 场景 | 防护 |
|------|------|
| 多核同时改 EDF 全局链 | `g_ready_lock` + 本地 `irq_save` |
| create/unblock 持 `g_klock` 时踢核 | 推迟 kick 至 `exit_critical` |
| 中断与任务并发 delay 链 | `process_delayed` 持 `g_klock`；入链前 remove |
| DPCP 改 deadline/prio 时任务 READY | 先出队再改再入队 |