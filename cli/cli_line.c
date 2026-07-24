/**
 * @file cli_line.c
 * @brief CLI 行编辑实现
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
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
 * @details 容量 CLI_HIST_MAX；g_hist_head 为下一写入下标；g_hist_len 为有效条数。
 */
static char g_hist[CLI_HIST_MAX][CLI_LINE_MAX];
/** @brief 历史有效条数（0..CLI_HIST_MAX） */
static int  g_hist_len;
/** @brief 下一写入位置（环形） */
static int  g_hist_head;
/** @brief 上翻历史前保存的当前草稿行 */
static char g_draft[CLI_LINE_MAX];
/** @brief 历史浏览下标；-1 表示编辑草稿而非历史 */
static int  g_hist_view = -1;

/**
 * @brief 有界字符串拷贝（保证 NUL 结尾）
 * @details 拷贝至多 cap-1 字节；src 为 NULL 时写空串；cap<1 则直接返回。
 * @param[out] dst 目标缓冲
 * @param[in]  src 源串；可为 NULL
 * @param[in]  cap 容量（含 NUL）
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
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

/**
 * @brief 计算 C 字符串长度
 * @details NULL 视为长度 0。
 * @param[in] s 输入串；可为 NULL
 * @return 字符数（不含 NUL）
 * @retval >=0 长度
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
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

/**
 * @brief 将已提交行推入历史（空行忽略）
 * @details 写入 g_hist 环形缓冲；覆盖最旧条目。
 * @param[in] line 行
 * @return 无
 * @retval 无
 * @note 保护：g_hist 无锁单任务
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
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

/**
 * @brief 按浏览下标取历史条目
 * @details view=0 为最近一条；越界返回 NULL。
 * @param[in] view 从近到远的下标（0..g_hist_len-1）
 * @return 历史串指针；无效则 NULL
 * @retval 非 NULL 指向 g_hist 槽
 * @retval NULL 越界
 * @note 返回指针指向静态表，勿 free
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
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
 * @brief 重绘整行：回车符 + prompt + 文本 + 清 EOL + 回退光标
 * @details 先 \r 再打印提示符与文本，ANSI 清行尾，必要时用 CSI 左移到 cursor。
 * @param[in] prompt 提示符；NULL 用默认
 * @param[in] text   行文本
 * @param[in] len    文本长度
 * @param[in] cursor 光标列（0..len）
 * @return 无
 * @retval 无
 * @note 依赖 ANSI 终端
 * @warning 吞掉当前行显示内容
 * @attention ❌ ISR；✅ block/switch
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

/**
 * @brief 从 UART 读入一行（可编辑）
 * @details 支持左右光标、Backspace/Delete、↑↓ 历史、Tab 路径补全、Ctrl-C 清空；Enter 提交。
 * @param[out] line     输出缓冲
 * @param[in]  line_cap 容量（须 >= 2，建议 CLI_LINE_MAX）
 * @param[in]  prompt   提示符；NULL 用默认
 * @return 0 正常提交；1 Ctrl-C；-1 参数错误
 * @retval 0 正常
 * @retval 1 用户 Ctrl-C（line 为空）
 * @retval -1 参数非法
 * @note 历史表无锁单任务；Tab→cli_path_complete
 * @warning 吞掉 ANSI CSI；与 vi 接管互斥（同任务）
 * @attention ❌ ISR；✅ block/switch
 */
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
