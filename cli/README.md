# CG-RTOS 交互式 CLI 应用

本目录为 `APP=cli` 源码（不再放在 `tests/`）。

| 模块 | 文件 |
|------|------|
| 入口 / 命令分发 | `cli_main.c` |
| 行编辑 + 历史 + Tab | `cli_line.*` |
| 路径 / CWD / 补全 | `cli_path.*` |
| 文件系统命令 | `cli_fs.*` |
| POSIX vi | `cli_vim.*` |

共享测试用例实现仍链接 `tests/test_cases.c`、`tests/stress_cases.c`（`run <case>`）。

文档：[docs/MODULE_CLI_VIM.md](../docs/MODULE_CLI_VIM.md)、[docs/USER_GUIDE.md](../docs/USER_GUIDE.md)。

```bash
./scripts/cgrtos.sh cli --cores 2
```
