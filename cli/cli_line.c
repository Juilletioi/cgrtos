/**
 * @file cli_line.c
 * @brief CLI 行编辑实现
 * @author Cong Zhou / Juilletioi
 * @version 5.1.0
 * @date 2026-07-23
 * @copyright CG-RTOS
 *
 * @details
 * ## 并发
 * - g_hist / g_draft / g_hist_*：无锁，cli 任务独占。
 * - Tab → cli_path_complete：可能持 g_fs_mtx（vfs）与 g_klock（malloc）；
 *   不在持锁期间阻塞读 UART。
 *
 * ## Ctrl-C
 * 清空当前行并返回 1（空串）；不中止已提交命令。
 */
#include "cli_line.h"
#include "cli_path.h"
#include <string.h>

/**
 * @brief 命令历史环（无锁；单 CLI 任务）
 */
static char g_hist[CLI_HIST_MAX][CLI_LINE_MAX];
static int  g_hist_len;
static int  g_hist_head;
static char g_draft[CLI_LINE_MAX];
static int  g_hist_view = -1;

static void line_strcpy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (!dst || cap < 1) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int line_strlen(const char *s)
{
    int n = 0;
    if (!s) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    return n;
}

void cli_line_hist_push(const char *line)
{
    if (!line || !line[0]) {
        return;
    }
    line_strcpy(g_hist[g_hist_head], line, CLI_LINE_MAX);
    g_hist_head = (g_hist_head + 1) % CLI_HIST_MAX;
    if (g_hist_len < CLI_HIST_MAX) {
        g_hist_len++;
    }
}

static const char *hist_get(int view)
{
    int idx;
    if (view < 0 || view >= g_hist_len) {
        return 0;
    }
    idx = g_hist_head - 1 - view;
    while (idx < 0) {
        idx += CLI_HIST_MAX;
    }
    return g_hist[idx];
}

/**
 * @brief 重绘整行：\\r + prompt + 文本 + 清 EOL + 回退光标
 * @internal
 */
static void line_redraw(const char *prompt, const char *text, int len, int cursor)
{
    int i;
    cgrtos_uart_putc('\r');
    cgrtos_uart_puts(prompt ? prompt : CLI_PROMPT_DEFAULT);
    for (i = 0; i < len; i++) {
        cgrtos_uart_putc(text[i]);
    }
    cgrtos_uart_puts("\033[K");
    /* 将光标移到 prompt+cursor */
    if (cursor < len) {
        cgrtos_printf("\033[%dD", len - cursor);
    }
}

int cli_line_read(char *line, int line_cap, const char *prompt)
{
    int len = 0;
    int cursor = 0;
    int esc = 0;
    int last_was_tab = 0;
    const char *pr = prompt ? prompt : CLI_PROMPT_DEFAULT;

    if (!line || line_cap < 2) {
        return -1;
    }
    line[0] = 0;
    g_hist_view = -1;
    g_draft[0] = 0;
    cgrtos_uart_puts(pr);

    while (1) {
        int ch = cgrtos_uart_pollc();
        if (ch < 0) {
            cgrtos_task_yield();
            continue;
        }

        if (esc == 1) {
            if (ch == '[') {
                esc = 2;
                continue;
            }
            esc = 0;
            last_was_tab = 0;
            continue;
        }
        if (esc == 2) {
            esc = 0;
            last_was_tab = 0;
            if (ch == 'A') {
                if (g_hist_view < 0) {
                    line[len] = 0;
                    line_strcpy(g_draft, line, CLI_LINE_MAX);
                }
                if (g_hist_view + 1 < g_hist_len) {
                    g_hist_view++;
                    {
                        const char *h = hist_get(g_hist_view);
                        if (h) {
                            line_strcpy(line, h, line_cap);
                            len = line_strlen(line);
                            cursor = len;
                            line_redraw(pr, line, len, cursor);
                        }
                    }
                }
                continue;
            }
            if (ch == 'B') {
                if (g_hist_view < 0) {
                    continue;
                }
                g_hist_view--;
                if (g_hist_view < 0) {
                    line_strcpy(line, g_draft, line_cap);
                    len = line_strlen(line);
                } else {
                    const char *h = hist_get(g_hist_view);
                    if (h) {
                        line_strcpy(line, h, line_cap);
                        len = line_strlen(line);
                    }
                }
                cursor = len;
                line_redraw(pr, line, len, cursor);
                continue;
            }
            if (ch == 'C') {
                /* Right */
                if (cursor < len) {
                    cursor++;
                    cgrtos_uart_puts("\033[C");
                }
                continue;
            }
            if (ch == 'D') {
                /* Left */
                if (cursor > 0) {
                    cursor--;
                    cgrtos_uart_puts("\033[D");
                }
                continue;
            }
            if (ch == '3') {
                /* Delete: ESC [ 3 ~ */
                int n2 = cgrtos_uart_pollc();
                if (n2 < 0) {
                    /* wait briefly */
                    for (int t = 0; t < 100 && n2 < 0; t++) {
                        cgrtos_task_yield();
                        n2 = cgrtos_uart_pollc();
                    }
                }
                if (n2 == '~' && cursor < len) {
                    for (int i = cursor; i < len - 1; i++) {
                        line[i] = line[i + 1];
                    }
                    len--;
                    line[len] = 0;
                    line_redraw(pr, line, len, cursor);
                }
                continue;
            }
            continue;
        }

        if (ch == 0x1B) {
            esc = 1;
            continue;
        }

        if (ch == '\t') {
            int list = last_was_tab ? 1 : 0;
            int r;
            last_was_tab = 1;
            g_hist_view = -1;
#if CONFIG_CLI_FS
            r = cli_path_complete(line, line_cap, &len, &cursor, list);
            if (list) {
                line_redraw(pr, line, len, cursor);
            } else if (r == 1) {
                line_redraw(pr, line, len, cursor);
            }
#else
            cgrtos_uart_putc('\a');
#endif
            continue;
        }
        last_was_tab = 0;

        if (ch == '\r' || ch == '\n') {
            cgrtos_uart_putc('\n');
            line[len] = 0;
            g_hist_view = -1;
            g_draft[0] = 0;
            return 0;
        }

        if (ch == 0x7F || ch == 0x08) {
            if (cursor > 0) {
                for (int i = cursor - 1; i < len - 1; i++) {
                    line[i] = line[i + 1];
                }
                len--;
                cursor--;
                line[len] = 0;
                line_redraw(pr, line, len, cursor);
            }
            g_hist_view = -1;
            continue;
        }

        if (ch == 0x03) {
            cgrtos_printf("^C\n");
            line[0] = 0;
            len = 0;
            cursor = 0;
            g_hist_view = -1;
            g_draft[0] = 0;
            return 1;
        }

        if (ch >= 32 && ch < 127 && len < line_cap - 1) {
            for (int i = len; i > cursor; i--) {
                line[i] = line[i - 1];
            }
            line[cursor] = (char)ch;
            len++;
            cursor++;
            line[len] = 0;
            line_redraw(pr, line, len, cursor);
            g_hist_view = -1;
        }
    }
}
