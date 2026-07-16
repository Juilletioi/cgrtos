/**
 * @file cli_main.c
 * @brief CG-RTOS 交互式 CLI 应用：串口命令跑测试 case
 *
 * 启动：
 *   ./scripts/cgrtos.sh cli
 *   make APP=cli && ./scripts/cgrtos.sh run --app cli --no-build
 *
 * 常用命令：help / list / run <case>|all|stress / stats / heap / ticks / cores
 */
#include "../kernel/cgrtos.h"
#include "test_cases.h"

#define CLI_LINE_MAX 96
#define CLI_PROMPT   "cgrtos> "

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

static void cli_help(void)
{
    cgrtos_printf("\nCG-RTOS CLI commands:\n");
    cgrtos_printf("  help              show this help\n");
    cgrtos_printf("  list | ls         list runnable test cases\n");
    cgrtos_printf("  run <case>|all|stress  run a named case\n");
    cgrtos_printf("  stats             dump kernel runtime stats\n");
    cgrtos_printf("  ticks             print current tick count\n");
    cgrtos_printf("  heap              print free / min-ever heap\n");
    cgrtos_printf("  cores             print SMP / LB counters\n");
    cgrtos_printf("  yield             yield CPU once\n");
    cgrtos_printf("  clear             clear screen (ANSI)\n");
    cgrtos_printf("\nExamples:\n");
    cgrtos_printf("  run mem\n");
    cgrtos_printf("  run sched\n");
    cgrtos_printf("  run stress\n");
    cgrtos_printf("  run all\n\n");
}

static void cli_handle(char *line)
{
    const char *arg = 0;
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
    } else if (cli_streq(line, "stats")) {
        cgrtos_stats_dump();
    } else if (cli_streq(line, "ticks")) {
        cgrtos_printf("ticks=%lu\n", (unsigned long)cgrtos_get_ticks());
    } else if (cli_streq(line, "heap")) {
        cgrtos_printf("free=%lu min_ever=%lu heap_size=%u\n",
                      cgrtos_get_free_heap(), cgrtos_get_min_free_heap(),
                      (unsigned)CONFIG_HEAP_SIZE);
    } else if (cli_streq(line, "cores")) {
        cgrtos_printf("secondary=%u cs=%u (c0=%u c1=%u) lb_mig=%u steal=%u\n",
                      g_secondary_online, g_cs_count,
                      g_cs_count_core[0], g_cs_count_core[1],
                      g_lb_migrate_count, g_lb_steal_count);
        cgrtos_printf("ready/load c0=%u/%u c1=%u/%u\n",
                      cgrtos_sched_ready_count(0), cgrtos_sched_core_load(0),
                      cgrtos_sched_ready_count(1), cgrtos_sched_core_load(1));
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

    /* Drain any stale RX bytes from QEMU/monitor. */
    while (cgrtos_uart_pollc() >= 0) {
    }

    cgrtos_printf("\n======== CG-RTOS Interactive CLI ========\n");
    cgrtos_printf("Type 'help' or 'list'. Run cases with 'run <name>'.\n");
    cgrtos_uart_puts(CLI_PROMPT);

    while (1) {
        int ch = cgrtos_uart_pollc();
        if (ch < 0) {
            cgrtos_task_yield();
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            cgrtos_uart_putc('\n');
            line[len] = 0;
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
            continue;
        }

        if (ch == 0x03) {
            cgrtos_printf("^C\n");
            len = 0;
            cgrtos_uart_puts(CLI_PROMPT);
            continue;
        }

        if (ch >= 32 && ch < 127 && len < CLI_LINE_MAX - 1) {
            line[len++] = (char)ch;
            cgrtos_uart_putc((char)ch);
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
