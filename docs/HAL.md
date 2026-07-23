# CG-RTOS HAL 外设驱动框架详解

版本：与内核同步（**5.3**）  
头文件：`hal/hal.h`（用户 API）· `boards/<BOARD>/hal_board.h`（板级 MMIO）  
实现：`hal/hal_core.c` + `hal/hal_compat.c` + `hal/hal_printf.c` + `arch/<ARCH>/*.c`  
相关：`docs/PORTING.md` · `docs/ARCHITECTURE.md` · `./scripts/cgrtos.sh sdk`

---

## 1. 目标与动机

HAL 将「稳定的用户接口」与「板级 MMIO + 驱动 ops」分离。历史 `cgrtos_*` 外设 API 保留为薄封装。

驱动导出使用中性名：`drv_timer_device` / `drv_irqc_device`（兼容 `drv_clint_device` / `drv_plic_device`）。

---

## 2. 分层模型

```
  App / Kernel / Tests
       |  推荐：hal_*        兼容：cgrtos_uart_* / …
       v
  HAL 用户 API（hal/hal.h）
       |  boot：drv_*_device() → register → init
       v
  Board drivers（arch/<ARCH>/*.c：只实现 ops + MMIO）
       |  MMIO 来自 boards/<BOARD>/hal_board.h；禁止调用 hal_*
       v
  硬件（Nuclei / SiFive / QEMU virt ARM64 …）

  Trap / 底层 ISR ──直调──> arch_handle_* / 驱动内部（禁止再进 hal_irqc_*）
```

**依赖方向：HAL → 驱动；驱动与 trap 不得调用 HAL 用户 API。**

启动：`cgrtos_init()` → `hal_board_init()`：

1. `drv_cpu/uart/irqc/timer/ipi_device()` 注册  
2. 按序初始化：CPU → Console → IRQC → Timer → IPI  
3. **冻结注册表**

换板：复制 `boards/<BOARD>/`，不必改内核。详见 [PORTING.md](PORTING.md)。

---

## 3. 错误码 `hal_status_t`

| 值 | 宏 | 含义 |
|----|-----|------|
| 0 | `HAL_OK` | 成功 |
| -1 | `HAL_ERR_PARAM` | 参数非法 |
| -2 | `HAL_ERR_NODEV` | 设备未注册或未就绪 |
| -3 | `HAL_ERR_IO` | 硬件 I/O 失败（预留） |
| -4 | `HAL_ERR_BUSY` | 资源忙（预留） |
| -5 | `HAL_ERR_STATE` | 状态不允许（如注册表已冻结） |

与内核 `pdPASS`(1) / `pdFAIL`(0) **不同**。兼容层负责转换，例如：

```c
return (hal_irqc_enable(irq) == HAL_OK) ? pdPASS : pdFAIL;
```

特殊：`hal_console_pollc()` 返回 `-1` 表示 RX 空，**不是** HAL 错误码。

---

## 4. API 速查

### 4.1 设备注册表

| API | 作用 |
|-----|------|
| `hal_device_register` | boot 注册；冻结后失败 |
| `hal_device_find` / `find_by_name` | 按类别 / 名查找 |
| `hal_device_count` / `get` | 枚举 |
| `hal_registry_frozen` | 是否已冻结 |
| `hal_board_init` | 板级一键初始化 |

### 4.2 控制台

| API | 作用 |
|-----|------|
| `hal_console_init` | 硬件初始化 |
| `hal_console_putc` | 单字符（内部加锁） |
| `hal_console_puts` / `write` | 整段持锁，行级近似原子 |
| `hal_console_lock` / `unlock` + `putc_unlocked` | 自定义批量输出 |
| `hal_console_pollc` / `getc` | 读；等待期间不持锁 |

`cgrtos_printf` 在整条消息期间持**控制台锁**（不是 `g_klock`）。

### 4.3 定时器 / 中断控制器 / IPI / CPU

| API | 说明 |
|-----|------|
| `hal_timer_init` / `hal_mtime_read` | 本核 tick；读 mtime |
| `hal_irqc_claim` / `complete` / `set|get_threshold` | 本 hart ISR 路径 |
| `hal_irqc_set_priority` / `enable` / `disable` | 配置路径（有配置锁） |
| `hal_ipi_send` / `hal_ipi_clear` | 软件 IPI |
| `hal_cpu_init` | 打开 MSIE \| MTIE \| MEIE |

---

## 5. 并发与 ISR 契约（必读）

| 操作 | Boot | 运行期多任务/多核 | ISR |
|------|------|-------------------|-----|
| 设备注册 | 仅 hart0 | **禁止** | 禁止 |
| console 输出 | 可用 | 有锁；puts/write/printf 整段原子 | 可用，勿长时间占锁 |
| mtime 读 | 可用 | 无锁（RV64 原子读） | 安全 |
| irqc claim/complete/threshold | — | 每 hart 独立寄存器 | **专供外部中断 ISR** |
| irqc enable/priority | 建议 boot 配好 | 配置锁保护 RMW | 勿在高频 ISR 改表 |
| ipi send/clear | — | 字写；非法 hart → `HAL_ERR_PARAM` | clear 通常在本核 ISR |

### 5.1 控制台锁

- 独立自旋锁 + 本核 `irq_save`，**刻意不使用** `g_klock`。  
  否则在 `cgrtos_enter_critical()` 内调用 `printf` 会死锁。
- 持锁期间禁止：`yield`、长时间阻塞、再取内核大锁。
- `getc` 在等待 RX 时不持锁，避免饿死打印方。

### 5.2 IRQC 配置锁

- `set_priority` / `enable` / `disable` 走配置锁（RMW 安全）。
- `claim` / `complete` / `threshold` **不加**配置锁，防止 ISR 与任务互相卡住。

### 5.3 注册表冻结

`hal_board_init` 成功后 `hal_registry_frozen()!=0`。之后再 `hal_device_register` 必须得到 `HAL_ERR_STATE`。热路径通过 `g_by_class[]` 缓存，避免反复线性查找。

---

## 6. 可移植性：如何换板

1. 复制并修改 `hal/hal_board.h`（或用 `-DHAL_BOARD_UART_BASE=...` 覆盖）。
2. 按 ops 表实现 / 调整 `arch/...` 驱动，导出 `drv_*_device()`（**不要**在驱动里调 `hal_*`）。
3. 在 `hal_board_init` 中保持注册顺序符合依赖（CPU → 可打印 → IRQC → Timer → IPI）。
4. **不要**改 `hal/hal.h` 的用户 API 语义（保持应用源码兼容）。

当前板级：`HAL_BOARD_NAME = "nuclei_evalsoc_ux900"`。

| 符号 | 典型用途 |
|------|----------|
| `HAL_BOARD_UART_BASE` | 控制台 |
| `HAL_BOARD_SYSTIMER_BASE` / `CLINT_BASE` | mtime / mtimecmp / MSIP |
| `HAL_BOARD_PLIC_*` | PLIC；注意 Nuclei M-mode context = `hart * 2` |

---

## 7. 应用示例

```c
#include "cgrtos.h"

void app_hal_demo(void)
{
    int n;

    hal_console_puts("hello HAL\n");
    n = hal_console_write("RAW", 3);
    (void)n;

    cgrtos_printf("mtime=%lx devices=%d frozen=%d\n",
                  (unsigned long)hal_mtime_read(),
                  hal_device_count(),
                  hal_registry_frozen());

    /* 批量输出：持锁 + unlocked putc */
    hal_console_lock();
    hal_console_putc_unlocked('[');
    hal_console_putc_unlocked(']');
    hal_console_unlock();

    /* 与历史 API 等价 */
    cgrtos_uart_puts("compat ok\n");
}
```

## 8. 与兼容层对照

| 兼容 API | HAL |
|----------|-----|
| `cgrtos_arch_init` | `hal_cpu_init` |
| `cgrtos_uart_init/putc/pollc/getc/puts` | `hal_console_*` |
| `cgrtos_printf` | 内部走控制台锁 + unlocked putc |
| `cgrtos_clint_init` / `cgrtos_mtime_read` | `hal_timer_init` / `hal_mtime_read` |
| `cgrtos_plic_*` | `hal_irqc_*`（返回值做 pdPASS/pdFAIL 映射） |
| `cgrtos_smp_send_ipi` | `hal_ipi_send` |

新代码优先用 `hal_*`；维护旧模块可继续用 `cgrtos_*`。

---

## 9. 测试与验收

```bash
./scripts/cgrtos.sh test              # 含 case:hal
./scripts/cgrtos.sh test --cores 1
./scripts/cgrtos.sh test --cores 4
./scripts/cgrtos.sh stress
./scripts/cgrtos.sh sdk               # 打包hal.h +hal_board.h + API HTML
```

CLI：`runhal` 或 `run all`。`case_hal` 覆盖：冻结注册、错误码、write/puts、irqc/ipi 边界、兼容层一致性。

---

## 10. 移植检查清单

- [ ] 所有 MMIO 基址仅出现在 `hal_board.h`（或 `-D` 覆盖）
- [ ] 驱动 `init` 返回 `HAL_OK` / `HAL_ERR_*`
- [ ] console 驱动 **不再**自行加锁（由 HAL 持锁调用 putc/pollc）
- [ ] claim/complete 路径不加配置锁
- [ ] `hal_board_init` 后注册表冻结；测试 `hal_reg_frozen` 通过
- [ ] 多核下 `printf` / `puts` 不出现半行交错（整段持锁）
- [ ] Doxygen / SDK 已用 `./scripts/cgrtos.sh sdk` 刷新
