/**
 * @file docs/MODULE_DBG_PERF.md
 * @brief 调试工具 / Trace / 配置裁剪 / BSP 可移植性（v5.1）
 */
# 调试、性能与可移植性（v5.1）

## 1. 分级日志与对象查询
- `cgrtos_log` / `CGRTOS_LOGE/W/I/D`：输出带 tick（`[I][tag][tN]`）
- `cgrtos_objects_stats_get` / `cgrtos_objects_dump` / `*_list_export`
- CLI：`objects`、`trace`（`APP=cli`）

## 2. Trace
- `CONFIG_USE_TRACE` + `CONFIG_TRACE_ENTRIES`（2 的幂）
- API：`cgrtos_trace_event` / `export` / `dump` / `reset`
- 自动埋点：ISR enter/exit、sched switch、block/unblock
- 写入路径：仅关本地 IRQ，不取 `g_klock`

## 3. 热路径
- 优先级就绪：bitmap + `__builtin_clz`（O(1)）
- `cgrtos_sched_switch_from_trap`：解锁后再做栈检查与 Trace，缩短 `g_ready_lock` 持有

## 4. 配置
- 全部开关集中于 `kernel/cgrtos_config.h`
- `make PROFILE=minimal` → `-DCGRTOS_CONFIG_MINIMAL=1`

## 5. BSP
- `boards/{BOARD}/hal_board.h`（默认 `nuclei_evalsoc`）
- `board_ipi_clear()` 替代 `startup.S` 硬编码 CLINT MSIP
- Makefile：`BOARD=nuclei_evalsoc`，`-Iboards/$(BOARD)`
