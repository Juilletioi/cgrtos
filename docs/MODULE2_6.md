# 模块2–6 增强摘要（CG-RTOS 5.0.0）

## 模块2 IPC
- 统一超时返回 `errTIMEOUT`；参数 `errPARAM`
- `CONFIG_ISR_API_GUARD`：阻塞 API 禁止在 ISR 调用 → `errISR` + 钩子
- `CONFIG_DETECT_DEADLOCK`：互斥等待环检测 → `errDEADLOCK`
- 既有：PI / DPCP / 全超时语义

**风险**：死锁检测仅覆盖 mutex owner 链，深度上限 16；持 `g_klock`。

## 模块3 内存
- `cgrtos_mempool_*` 固定块池（无碎片）
- `CONFIG_HEAP_POISON` 分配/释放喷毒
- `CONFIG_HEAP_REDZONE` 用户区尾金丝雀越界检测
- 既有 TLSF magic + double-free 软拒绝 + 栈 HWM

**风险**：红区写入依赖块尾部余量；临界区内 alloc/free。

## 模块4 安全
- 任务创建/删除、ISR API、调度错误、异常、看门狗、关中断超时钩子
- `CONFIG_IRQ_DISABLE_MONITOR` 监控临界区时长
- `cgrtos_fatal_error` / `cgrtos_watchdog_kick`
- `CONFIG_USE_MPU` 软件桩（区域配置 / 任务隔离接口）

**风险**：关中断监控基于 mtime，QEMU 下阈值宜放宽。

## 模块5 性能
- 既有优先级位图 O(1)
- `CONFIG_USE_EDF_HEAP`：EDF 就绪最小堆
- `cgrtos_set_idle_sleep_hook` + `CONFIG_IDLE_BUSY_PUMP=0` 时 WFI

## 模块6 调试
- `CONFIG_USE_KLOG` / `CGRTOS_LOGx` / `cgrtos_log_set_level`
- `cgrtos_task_list_export`
- 错误码：`errTIMEOUT/PARAM/NO_MEM/ISR/DEADLOCK/OVERFLOW/STATE`

## 模块7 测试
- `m2_ipc` / `m3_mem` / `m4_safe` / `m5_perf` / `m6_dbg` 用例
