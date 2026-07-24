/**
 * @file cli_main.c
 * @brief CG-RTOS 交互式 CLI：跑测试 case、读写内存、命令历史上翻
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * 启动：`./scripts/cgrtos.sh cli`。
 * 常用：help / list / run \<case\> / md|mw / stats / heap / ticks / cores。
 * 行编辑：Backspace、左右光标、Ctrl-C、↑↓ 历史、Tab 路径补全；vi/vim/edit 见 MODULE_CLI_VIM.md。
 */
#include "../kernel/cgrtos.h"
#include "../tests/test_cases.h"
#include "cli_fs.h"
#include "cli_line.h"
#include "cli_vim.h"

/**
 * @brief 默认 CLI 提示符字符串
 * @warning 无运行时副作用（编译期常量）
 */
#define CLI_PROMPT    "cgrtos> "
/**
 * @brief 单次 md/mw 最大单元数上限
 * @warning 无运行时副作用（编译期常量）
 */
#define CLI_MD_MAX    256


/**
 * @brief 就地去除字符串首尾空白
 * @details 左移跳过前导空格/制表符，再剥除尾部空白与 CR/LF。
 * @param[in,out] s 以 NUL 结尾的可写行缓冲
 * @return 无
 * @retval 无
 * @note 原地修改，不分配内存
 * @warning s 须可写且以 NUL 结尾
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static void cli_trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        char *d = s;
        while (*p) {
            *d++ = *p++;
        }
        *d = 0;
    }
    int n = 0;
    while (s[n]) {
        n++;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                      s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = 0;
    }
}

/**
 * @brief 比较两 C 字符串是否完全相等
 * @details 逐字节比较至双 NUL。
 * @param[in] a 首串
 * @param[in] b 次串
 * @return 相等为 1，否则 0
 * @retval 1 内容一致
 * @retval 0 不等或指针差异
 * @note 区分大小写
 * @warning 指针须有效或同为 NULL
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static int cli_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/**
 * @brief 判断 s 是否以 prefix 开头且其后为空白或结束
 * @details 匹配 prefix 后跳过空白，可选写出剩余参数字符串指针。
 * @param[in]  s      待测完整行
 * @param[in]  prefix 命令前缀（如 "run"）
 * @param[out] rest   非 NULL 时写入参数起始；可为 NULL
 * @return 匹配成功 1，否则 0
 * @retval 1 prefix 匹配且边界合法
 * @retval 0 不匹配
 * @note 用于 `run foo` 类命令解析
 * @warning rest 指向 s 内部，勿 free
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static int cli_startswith(const char *s, const char *prefix, const char **rest)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    if (*s == 0 || *s == ' ' || *s == '\t') {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (rest) {
            *rest = s;
        }
        return 1;
    }
    return 0;
}

/**
 * @brief 解析无符号整数（十进制或 0x 十六进制）
 * @details 支持 0x/0X 前缀；成功时写入 *out，可选 *endp 指向首个非数字字符。
 * @param[in]  s    输入串
 * @param[out] out  解析结果
 * @param[out] endp 非 NULL 时写入结束位置；可为 NULL
 * @return 0 成功，-1 失败
 * @retval 0  至少一位有效数字
 * @retval -1 空串或无数字
 * @note 用于 md/mw 地址与数值
 * @warning 不检测溢出
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static int cli_parse_u64(const char *s, uint64_t *out, const char **endp)
{
    uint64_t v = 0;
    int base = 10;
    int digits = 0;

    if (!s || !*s) {
        return -1;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    while (*s) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        if (base == 16) {
            v = (v << 4) | (uint64_t)d;
        } else {
            v = v * 10u + (uint64_t)d;
        }
        s++;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    *out = v;
    if (endp) {
        *endp = s;
    }
    return 0;
}

/**
 * @brief 跳过前导空格与制表符
 * @details 返回首个非空白字符指针，或指向 NUL。
 * @param[in] s 输入串
 * @return 跳过空白后的指针
 * @retval 非 NULL 始终有效（同 s 或其后）
 * @note 不修改输入
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static const char *cli_skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/**
 * @brief 匹配 md/read 内存 dump 命令并解析宽度后缀
 * @details 支持 `md[.b|.h|.w|.d]` 与 `read[...]`；默认宽度 4 字节。
 * @param[in]  cmd      完整命令行
 * @param[out] width    元素宽度（1/2/4/8）
 * @param[out] rest_out 非 NULL 时写入参数起始
 * @return 0 匹配成功，-1 不匹配或语法错误
 * @retval 0  已设置 width 与 rest
 * @retval -1 非 md/read 或非法后缀
 * @note .b=1 .h=2 .w=4 .d=8
 * @warning cmd 须以 NUL 结尾
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static int cli_match_md(const char *cmd, int *width, const char **rest_out)
{
    const char *p;
    int w = 4;

    if (cmd[0] == 'm' && cmd[1] == 'd') {
        p = cmd + 2;
    } else if (cmd[0] == 'r' && cmd[1] == 'e' && cmd[2] == 'a' && cmd[3] == 'd') {
        p = cmd + 4;
    } else {
        return -1;
    }
    if (*p == '.') {
        p++;
        if (*p == 'b') {
            w = 1;
            p++;
        } else if (*p == 'h') {
            w = 2;
            p++;
        } else if (*p == 'w') {
            w = 4;
            p++;
        } else if (*p == 'd') {
            w = 8;
            p++;
        } else {
            return -1;
        }
    }
    if (*p != 0 && *p != ' ' && *p != '\t') {
        return -1;
    }
    *width = w;
    if (rest_out) {
        *rest_out = cli_skip_ws(p);
    }
    return 0;
}

/**
 * @brief 匹配 mw/write 内存写入命令并解析宽度后缀
 * @details 支持 `mw[.b|.h|.w|.d]` 与 `write[...]`；默认宽度 4 字节。
 * @param[in]  cmd      完整命令行
 * @param[out] width    元素宽度（1/2/4/8）
 * @param[out] rest_out 非 NULL 时写入参数起始
 * @return 0 匹配成功，-1 不匹配或语法错误
 * @retval 0  已设置 width 与 rest
 * @retval -1 非 mw/write 或非法后缀
 * @note 与 cli_match_md 对称
 * @warning 直接写物理/映射地址，慎用
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static int cli_match_mw(const char *cmd, int *width, const char **rest_out)
{
    const char *p;
    int w = 4;

    if (cmd[0] == 'm' && cmd[1] == 'w') {
        p = cmd + 2;
    } else if (cmd[0] == 'w' && cmd[1] == 'r' && cmd[2] == 'i' &&
               cmd[3] == 't' && cmd[4] == 'e') {
        p = cmd + 5;
    } else {
        return -1;
    }
    if (*p == '.') {
        p++;
        if (*p == 'b') {
            w = 1;
            p++;
        } else if (*p == 'h') {
            w = 2;
            p++;
        } else if (*p == 'w') {
            w = 4;
            p++;
        } else if (*p == 'd') {
            w = 8;
            p++;
        } else {
            return -1;
        }
    }
    if (*p != 0 && *p != ' ' && *p != '\t') {
        return -1;
    }
    *width = w;
    if (rest_out) {
        *rest_out = cli_skip_ws(p);
    }
    return 0;
}

/**
 * @brief 执行内存 dump 子命令
 * @details 解析地址与可选 count，按 width 逐单元 volatile 读取并格式化输出。
 * @param[in] args  参数字符串（addr [count]）
 * @param[in] width 单元字节宽（1/2/4/8）
 * @return 无
 * @retval 无
 * @note count 默认 16，上限 CLI_MD_MAX
 * @warning 未对齐地址仅警告仍继续读
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cli_cmd_md(const char *args, int width)
{
    uint64_t addr = 0, count = 16;
    const char *p = cli_skip_ws(args);
    const char *e;

    if (cli_parse_u64(p, &addr, &e) != 0) {
        cgrtos_printf("usage: md[.b|.h|.w|.d]|read <addr> [count]\n");
        return;
    }
    p = cli_skip_ws(e);
    if (*p) {
        if (cli_parse_u64(p, &count, &e) != 0 || count == 0) {
            cgrtos_printf("bad count\n");
            return;
        }
    }
    if (count > CLI_MD_MAX) {
        count = CLI_MD_MAX;
    }
    if (addr % (uint64_t)width) {
        cgrtos_printf("warn: addr 0x%lx not aligned to %d\n",
                      (unsigned long)addr, width);
    }

    for (uint64_t i = 0; i < count; i++) {
        uintptr_t a = (uintptr_t)(addr + i * (uint64_t)width);
        if ((i % (16u / (unsigned)width)) == 0) {
            if (i) {
                cgrtos_uart_putc('\n');
            }
            cgrtos_printf("  %p:", (void *)a);
        }
        if (width == 1) {
            uint8_t v = *(volatile uint8_t *)a;
            cgrtos_printf(" %c%c",
                          "0123456789abcdef"[(v >> 4) & 0xF],
                          "0123456789abcdef"[v & 0xF]);
        } else if (width == 2) {
            uint16_t v = *(volatile uint16_t *)a;
            cgrtos_printf(" %x", (unsigned)v);
        } else if (width == 4) {
            uint32_t v = *(volatile uint32_t *)a;
            cgrtos_printf(" %x", (unsigned)v);
        } else {
            uint64_t v = *(volatile uint64_t *)a;
            cgrtos_printf(" %lx", (unsigned long)v);
        }
    }
    cgrtos_uart_putc('\n');
}

/**
 * @brief 执行内存写入子命令
 * @details 解析起始地址与连续数值，按 width 写入并打印每项结果。
 * @param[in] args  参数字符串（addr val [val...]）
 * @param[in] width 单元字节宽（1/2/4/8）
 * @return 无
 * @retval 无
 * @note 最多写入 CLI_MD_MAX 个单元
 * @warning 直接写内存，错误地址可能 HardFault
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cli_cmd_mw(const char *args, int width)
{
    uint64_t addr = 0, val = 0;
    const char *p = cli_skip_ws(args);
    const char *e;
    int nwritten = 0;

    if (cli_parse_u64(p, &addr, &e) != 0) {
        cgrtos_printf("usage: mw[.b|.h|.w|.d]|write <addr> <val> [val...]\n");
        return;
    }
    p = cli_skip_ws(e);
    if (!*p) {
        cgrtos_printf("usage: mw[.b|.h|.w|.d]|write <addr> <val> [val...]\n");
        return;
    }
    if (addr % (uint64_t)width) {
        cgrtos_printf("warn: addr 0x%lx not aligned to %d\n",
                      (unsigned long)addr, width);
    }

    while (*p) {
        if (cli_parse_u64(p, &val, &e) != 0) {
            cgrtos_printf("bad value near '%s'\n", p);
            break;
        }
        uintptr_t a = (uintptr_t)(addr + (uint64_t)nwritten * (uint64_t)width);
        if (width == 1) {
            *(volatile uint8_t *)a = (uint8_t)val;
        } else if (width == 2) {
            *(volatile uint16_t *)a = (uint16_t)val;
        } else if (width == 4) {
            *(volatile uint32_t *)a = (uint32_t)val;
        } else {
            *(volatile uint64_t *)a = val;
        }
        cgrtos_printf("  [%p] <- 0x%lx\n", (void *)a, (unsigned long)val);
        nwritten++;
        p = cli_skip_ws(e);
        if (nwritten >= CLI_MD_MAX) {
            break;
        }
    }
}

/**
 * @brief 打印 CLI 帮助文本
 * @details 列出命令、键位、宽度后缀与示例。
 * @return 无
 * @retval 无
 * @note 由 help / ? 触发
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cli_help(void)
{
    cgrtos_printf("\nCG-RTOS CLI commands:\n");
    cgrtos_printf("  help              show this help\n");
    cgrtos_printf("  list              list runnable test cases\n");
    cgrtos_printf("  run <case>|all|stress  run a named case\n");
    cgrtos_printf("  md[.b|.h|.w|.d] <addr> [count]   memory dump (alias: read)\n");
    cgrtos_printf("  mw[.b|.h|.w|.d] <addr> <val>...  memory write (alias: write)\n");
    cgrtos_printf("  stats             dump kernel runtime stats\n");
    cgrtos_printf("  objects           dump object-pool usage\n");
    cgrtos_printf("  trace             dump recent kernel trace events\n");
    cgrtos_printf("  ticks             print current tick count\n");
    cgrtos_printf("  heap              print free / min-ever heap\n");
    cgrtos_printf("  cores             print SMP / LB counters\n");
    cgrtos_printf("  yield             yield CPU once\n");
    cgrtos_printf("  clear             clear screen (ANSI)\n");
    cli_fs_help();
    cli_vim_help();
    cgrtos_printf("\nKeys: ←→ cursor  ↑↓ history  Tab path complete  Backspace  Ctrl-C\n");
    cgrtos_printf("Widths: .b=1 .h=2 .w=4(default) .d=8  addr/val: 0x.. or decimal\n");
    cgrtos_printf("Editor: vi|vim|edit <file>  (see docs/MODULE_CLI_VIM.md)\n");
    cgrtos_printf("\nExamples:\n");
    cgrtos_printf("  list\n");
    cgrtos_printf("  run mem\n");
    cgrtos_printf("  mkdir /tmp ; touch /tmp/a.txt ; vi /tmp/a.txt\n");
    cgrtos_printf("  cat /tmp/a  <Tab>\n");
    cgrtos_printf("  md 0xA0000000 8\n");
    cgrtos_printf("  mw.w 0x80000000 0xdeadbeef\n\n");
}

/**
 * @brief 解析并分发一行 CLI 命令
 * @details trim 后匹配 help/list/run/md/mw/stats/ticks/heap/cores/yield/clear 及 FS/vim。
 * @param[in,out] line 可写行缓冲；会被 trim 原地修改
 * @return 无
 * @retval 无
 * @note 空行直接返回；未知命令提示 try help
 * @warning run/md/mw 可能长时间阻塞或写内存
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cli_handle(char *line)
{
    const char *arg = 0;
    int width = 4;

    cli_trim(line);
    if (line[0] == 0) {
        return;
    }

    if (cli_streq(line, "help") || cli_streq(line, "?")) {
        cli_help();
    } else if (cli_fs_try_handle(line)) {
        /* filesystem commands (CONFIG_CLI_FS); ls is directory listing */
    } else if (cli_streq(line, "list")) {
        test_cases_list();
    } else if (cli_startswith(line, "run", &arg)) {
        if (!arg || arg[0] == 0) {
            cgrtos_printf("usage: run <case>|all\n");
            test_cases_list();
        } else if (test_cases_run(arg) != 0) {
            cgrtos_printf("unknown case '%s' — try 'list'\n", arg);
        }
    } else if (cli_match_md(line, &width, &arg) == 0) {
        cli_cmd_md(arg, width);
    } else if (cli_match_mw(line, &width, &arg) == 0) {
        cli_cmd_mw(arg, width);
    } else if (cli_streq(line, "stats")) {
        cgrtos_stats_dump();
    } else if (cli_streq(line, "objects")) {
        cgrtos_objects_dump();
    } else if (cli_streq(line, "trace")) {
        cgrtos_trace_dump();
    } else if (cli_streq(line, "ticks")) {
        cgrtos_printf("ticks=%lu\n", (unsigned long)cgrtos_get_ticks());
    } else if (cli_streq(line, "heap")) {
        cgrtos_printf("free=%lu min_ever=%lu heap_size=%u\n",
                      cgrtos_get_free_heap(), cgrtos_get_min_free_heap(),
                      (unsigned)CONFIG_HEAP_SIZE);
    } else if (cli_streq(line, "cores")) {
        cgrtos_printf("secondary_mask=0x%x cs=%u lb_mig=%u steal=%u cores=%d\n",
                      g_secondary_online, g_cs_count,
                      g_lb_migrate_count, g_lb_steal_count,
                      CONFIG_NUM_CORES);
        for (int c = 0; c < CONFIG_NUM_CORES; c++) {
            cgrtos_printf("  c%d cs=%u ready=%u load=%u\n",
                          c, g_cs_count_core[c],
                          cgrtos_sched_ready_count((uint8_t)c),
                          cgrtos_sched_core_load((uint8_t)c));
        }
    } else if (cli_streq(line, "yield")) {
        cgrtos_task_yield();
        cgrtos_printf("yielded\n");
    } else if (cli_streq(line, "clear")) {
        cgrtos_printf("\033[2J\033[H");
    } else {
        cgrtos_printf("unknown command '%s' — try 'help'\n", line);
    }
}

/**
 * @brief CLI 主循环任务：行编辑与命令执行
 * @details 使用 cli_line_read（历史/光标/Tab）；提交后 hist_push 与 cli_handle。
 * @param[in] arg 未使用
 * @return 无（永不返回）
 * @retval 无
 * @note 优先级 12；启动前清空 RX；FS/vim 状态仅本任务访问
 * @warning 单任务独占交互
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cli_task(void *arg)
{
    (void)arg;
    char line[CLI_LINE_MAX];

    while (cgrtos_uart_pollc() >= 0) {
    }

    cli_fs_session_init();

    cgrtos_printf("\n======== CG-RTOS Interactive CLI ========\n");
    cgrtos_printf("Type 'help'. Tab completes paths. vi <file> edits.\n");

    while (1) {
        int rc = cli_line_read(line, CLI_LINE_MAX, CLI_PROMPT);
        if (rc < 0) {
            cgrtos_printf("cli: readline error\n");
            continue;
        }
        if (rc == 1) {
            /* Ctrl-C → empty line already printed */
            continue;
        }
        cli_line_hist_push(line);
        cli_handle(line);
    }
}

/**
 * @brief hart0 CLI 应用入口
 * @details cgrtos_init → 创建 cli 任务并绑核 0 → cgrtos_start。
 * @param[in] hartid 核号
 * @param[in] fdt    设备树（忽略）
 * @param[in] end    DDR/链接末地址提示
 * @return 正常不返回；异常路径可能返回 0
 * @retval 0 仅异常
 * @note 任务 ID 假定 create 后为 1
 * @warning 若 create 失败仍 start，CLI 不会运行
 * @attention ❌ ISR；✅ block/switch
 */
int main(int hartid, void *fdt, void *end)
{
    (void)fdt;
    (void)end;

    cgrtos_init();
    cgrtos_printf("  [BOOT] Hart %d — CLI (DDR end %p)\n", hartid, end);

    cgrtos_task_create("cli", cli_task, 0, 12, SCHED_PRIORITY);
    cgrtos_task_set_affinity(1, 0);
    cgrtos_start();
    return 0;
}

/**
 * @brief 次核入口
 * @details 转调 cgrtos_start_secondary。
 * @param[in] hartid 次核编号
 * @return 无（不返回）
 * @retval 无
 * @note CLI 交互仅在 hart0 cli 任务
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
void secondary_main(int hartid)
{
    cgrtos_start_secondary(hartid);
}
