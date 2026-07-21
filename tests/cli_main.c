/**
 * @file cli_main.c
 * @brief CG-RTOS 交互式 CLI：跑测试 case、读写内存、命令历史上翻
 *
 * 启动：
 *   ./scripts/cgrtos.sh cli
 *
 * 常用：help / list / run <case> / md|mw / stats / heap / ticks / cores
 * 编辑：Backspace、Ctrl-C、↑ 上一条、↓ 下一条
 */
#include "../kernel/cgrtos.h"
#include "test_cases.h"

#define CLI_LINE_MAX  96
#define CLI_HIST_MAX  16
#define CLI_PROMPT    "cgrtos> "
#define CLI_MD_MAX    256   /* max units per dump */

static char g_hist[CLI_HIST_MAX][CLI_LINE_MAX];
static int  g_hist_len;          /* entries stored (0..CLI_HIST_MAX) */
static int  g_hist_head;         /* next write slot (ring) */
static char g_draft[CLI_LINE_MAX]; /* line being edited before ↑ */
static int  g_hist_view = -1;    /* -1 = draft; 0 = newest recalled */

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

static int cli_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

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

static void cli_strcpy(char *dst, const char *src, int max)
{
    int i = 0;
    if (max <= 0) {
        return;
    }
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int cli_strlen(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Parse unsigned (0xhex or decimal). Returns 0 on success. */
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

static const char *cli_skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static void cli_hist_push(const char *line)
{
    if (!line || !line[0]) {
        return;
    }
    /* Skip duplicate consecutive entry. */
    if (g_hist_len > 0) {
        int last = (g_hist_head + CLI_HIST_MAX - 1) % CLI_HIST_MAX;
        if (cli_streq(g_hist[last], line)) {
            return;
        }
    }
    cli_strcpy(g_hist[g_hist_head], line, CLI_LINE_MAX);
    g_hist_head = (g_hist_head + 1) % CLI_HIST_MAX;
    if (g_hist_len < CLI_HIST_MAX) {
        g_hist_len++;
    }
}

static const char *cli_hist_get(int view)
{
    /* view 0 = newest, 1 = previous, ... */
    if (view < 0 || view >= g_hist_len) {
        return 0;
    }
    int idx = (g_hist_head + CLI_HIST_MAX - 1 - view) % CLI_HIST_MAX;
    return g_hist[idx];
}

static void cli_redraw(const char *text, int len)
{
    cgrtos_uart_putc('\r');
    cgrtos_uart_puts(CLI_PROMPT);
    cgrtos_uart_puts("\033[K");
    for (int i = 0; i < len; i++) {
        cgrtos_uart_putc(text[i]);
    }
}

/** Match "md[.b|.h|.w|.d]" / "read[.b|.h|.w|.d]" and return args. */
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

/** Match "mw[.b|.h|.w|.d]" / "write[.b|.h|.w|.d]". */
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

static void cli_help(void)
{
    cgrtos_printf("\nCG-RTOS CLI commands:\n");
    cgrtos_printf("  help              show this help\n");
    cgrtos_printf("  list | ls         list runnable test cases\n");
    cgrtos_printf("  run <case>|all|stress  run a named case\n");
    cgrtos_printf("  md[.b|.h|.w|.d] <addr> [count]   memory dump (alias: read)\n");
    cgrtos_printf("  mw[.b|.h|.w|.d] <addr> <val>...  memory write (alias: write)\n");
    cgrtos_printf("  stats             dump kernel runtime stats\n");
    cgrtos_printf("  ticks             print current tick count\n");
    cgrtos_printf("  heap              print free / min-ever heap\n");
    cgrtos_printf("  cores             print SMP / LB counters\n");
    cgrtos_printf("  yield             yield CPU once\n");
    cgrtos_printf("  clear             clear screen (ANSI)\n");
    cgrtos_printf("\nKeys: ↑ previous command  ↓ next  Backspace  Ctrl-C\n");
    cgrtos_printf("Widths: .b=1 .h=2 .w=4(default) .d=8  addr/val: 0x.. or decimal\n");
    cgrtos_printf("\nExamples:\n");
    cgrtos_printf("  list\n");
    cgrtos_printf("  run mem\n");
    cgrtos_printf("  run sched\n");
    cgrtos_printf("  run streambuf\n");
    cgrtos_printf("  run all\n");
    cgrtos_printf("  run stress\n");
    cgrtos_printf("  md 0xA0000000 8\n");
    cgrtos_printf("  md.b 0xA0000000 32\n");
    cgrtos_printf("  mw.w 0x80000000 0xdeadbeef\n");
    cgrtos_printf("  write 0x80000000 1 2 3\n\n");
}

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
    } else if (cli_streq(line, "list") || cli_streq(line, "ls")) {
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

static void cli_task(void *arg)
{
    (void)arg;
    char line[CLI_LINE_MAX];
    int len = 0;
    int esc = 0; /* 0=normal 1=got ESC 2=got ESC[ */

    while (cgrtos_uart_pollc() >= 0) {
    }

    cgrtos_printf("\n======== CG-RTOS Interactive CLI ========\n");
    cgrtos_printf("Type 'help' / 'list' / 'run <case>'. ↑ recalls last command.\n");
    cgrtos_uart_puts(CLI_PROMPT);

    while (1) {
        int ch = cgrtos_uart_pollc();
        if (ch < 0) {
            cgrtos_task_yield();
            continue;
        }

        /* ANSI CSI: ESC [ A/B (up/down) */
        if (esc == 1) {
            if (ch == '[') {
                esc = 2;
                continue;
            }
            esc = 0;
            /* fall through — orphan ESC byte ignored */
            continue;
        }
        if (esc == 2) {
            esc = 0;
            if (ch == 'A') {
                /* Up */
                if (g_hist_view < 0) {
                    line[len] = 0;
                    cli_strcpy(g_draft, line, CLI_LINE_MAX);
                }
                if (g_hist_view + 1 < g_hist_len) {
                    g_hist_view++;
                    const char *h = cli_hist_get(g_hist_view);
                    if (h) {
                        cli_strcpy(line, h, CLI_LINE_MAX);
                        len = cli_strlen(line);
                        cli_redraw(line, len);
                    }
                }
                continue;
            }
            if (ch == 'B') {
                /* Down */
                if (g_hist_view < 0) {
                    continue;
                }
                g_hist_view--;
                if (g_hist_view < 0) {
                    cli_strcpy(line, g_draft, CLI_LINE_MAX);
                    len = cli_strlen(line);
                } else {
                    const char *h = cli_hist_get(g_hist_view);
                    if (h) {
                        cli_strcpy(line, h, CLI_LINE_MAX);
                        len = cli_strlen(line);
                    }
                }
                cli_redraw(line, len);
                continue;
            }
            /* Other CSI sequences: ignore */
            continue;
        }

        if (ch == 0x1B) {
            esc = 1;
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            cgrtos_uart_putc('\n');
            line[len] = 0;
            cli_hist_push(line);
            g_hist_view = -1;
            g_draft[0] = 0;
            cli_handle(line);
            len = 0;
            cgrtos_uart_puts(CLI_PROMPT);
            continue;
        }

        if (ch == 0x7F || ch == 0x08) {
            if (len > 0) {
                len--;
                cgrtos_uart_puts("\b \b");
            }
            g_hist_view = -1;
            continue;
        }

        if (ch == 0x03) {
            cgrtos_printf("^C\n");
            len = 0;
            g_hist_view = -1;
            g_draft[0] = 0;
            cgrtos_uart_puts(CLI_PROMPT);
            continue;
        }

        if (ch >= 32 && ch < 127 && len < CLI_LINE_MAX - 1) {
            line[len++] = (char)ch;
            cgrtos_uart_putc((char)ch);
            g_hist_view = -1;
        }
    }
}

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

void secondary_main(int hartid)
{
    cgrtos_start_secondary(hartid);
}
