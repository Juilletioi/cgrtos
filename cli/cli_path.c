/**
 * @file cli_path.c
 * @brief CLI 路径会话与 Tab 补全实现
 * @author Cong Zhou / Juilletioi
 * @version 5.1.0
 * @date 2026-07-23
 * @copyright CG-RTOS
 *
 * @details
 * ## 并发保护
 * - g_sess：无锁，约定仅 cli 任务访问。
 * - vfs_*：内部 g_fs_mtx（非递归）；本文件在持锁回调外打印/malloc。
 * - cgrtos_malloc：g_klock；禁止持 g_fs_mtx 时调用。
 *
 * ## 死锁 / 抢占
 * - 禁止：持 g_fs_mtx → printf/getc/yield（会饿死其他 FS 用户）。
 * - 禁止：持 g_fs_mtx → malloc（可能再进 g_klock / 日志）。
 * - 补全与 vi 同属 cli 任务，不会与 fs_abort_poll 并行。
 */
#include "cli_path.h"

#if CONFIG_CLI_FS

#include "../kernel/vfs.h"
#include <string.h>

#define CLI_PATH_SESS_MAX   4
#define CLI_PATH_MATCH_MAX  64

typedef struct {
    uint8_t   used;
    task_id_t tid;
    char      cwd[CLI_FS_PATH_MAX];
} cli_path_sess_t;

/**
 * @brief 每任务 CWD 会话表
 * @details 保护方式：无锁；单 CLI 任务独占。多任务并发写属未定义行为。
 */
static cli_path_sess_t g_sess[CLI_PATH_SESS_MAX];

static int path_streq(const char *a, const char *b)
{
    if (!a || !b) {
        return a == b;
    }
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/**
 * @brief 取当前任务会话槽（必要时分配，CWD=/）
 * @details 保护：g_sess 无锁（单任务）。
 * @return 会话指针；表满 NULL
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
 */
static cli_path_sess_t *path_sess_get(void)
{
    task_id_t tid = 0;
    int cpu = (int)arch_cpu_id();
    cgrtos_task_t *cur = (cpu >= 0 && cpu < CONFIG_NUM_CORES) ? g_current[cpu] : 0;
    if (cur) {
        tid = cur->id;
    }
    for (int i = 0; i < CLI_PATH_SESS_MAX; i++) {
        if (g_sess[i].used && g_sess[i].tid == tid) {
            return &g_sess[i];
        }
    }
    for (int i = 0; i < CLI_PATH_SESS_MAX; i++) {
        if (!g_sess[i].used) {
            g_sess[i].used = 1;
            g_sess[i].tid = tid;
            g_sess[i].cwd[0] = '/';
            g_sess[i].cwd[1] = 0;
            return &g_sess[i];
        }
    }
    return 0;
}

void cli_path_session_init(void)
{
    cli_path_sess_t *s = path_sess_get();
    if (s) {
        s->cwd[0] = '/';
        s->cwd[1] = 0;
    }
}

int cli_path_normalize(const char *in, char *out, size_t out_sz)
{
    const char *p;
    char stack[16][CGRTOS_FS_MAX_NAME];
    int sp = 0;

    if (!in || !out || out_sz < 2 || in[0] != '/') {
        return -1;
    }
    p = in;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (*p == 0) {
            break;
        }
        {
            size_t n = 0;
            char comp[CGRTOS_FS_MAX_NAME];
            while (p[n] && p[n] != '/') {
                if (p[n] < 32 || p[n] == 127) {
                    return -1;
                }
                if (n + 1 >= sizeof(comp)) {
                    return -1;
                }
                comp[n] = p[n];
                n++;
            }
            comp[n] = 0;
            p += n;
            if (comp[0] == 0 || (comp[0] == '.' && comp[1] == 0)) {
                continue;
            }
            if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
                if (sp > 0) {
                    sp--;
                }
                continue;
            }
            if (sp >= 16) {
                return -1;
            }
            strncpy(stack[sp], comp, CGRTOS_FS_MAX_NAME);
            stack[sp][CGRTOS_FS_MAX_NAME - 1] = 0;
            sp++;
        }
    }
    if (sp == 0) {
        out[0] = '/';
        out[1] = 0;
        return 0;
    }
    {
        size_t o = 0;
        for (int i = 0; i < sp; i++) {
            size_t cl = strlen(stack[i]);
            if (o + 1 + cl + 1 > out_sz) {
                return -1;
            }
            out[o++] = '/';
            memcpy(out + o, stack[i], cl);
            o += cl;
        }
        out[o] = 0;
    }
    return 0;
}

int cli_path_resolve(const char *user, char *abs, size_t abs_sz, int quiet)
{
    cli_path_sess_t *s = path_sess_get();
    char joined[CLI_FS_PATH_MAX];

    if (!s || !abs || abs_sz < 2) {
        return -1;
    }
    if (!user || user[0] == 0) {
        strncpy(abs, s->cwd, abs_sz);
        abs[abs_sz - 1] = 0;
        return 0;
    }
    if (user[0] == '/') {
        if (cli_path_normalize(user, abs, abs_sz) != 0) {
            if (!quiet) {
                cgrtos_printf("fs: invalid path '%s'\n", user);
            }
            return -1;
        }
        return 0;
    }
    if (path_streq(s->cwd, "/")) {
        if (cgrtos_snprintf(joined, sizeof(joined), "/%s", user) < 0) {
            return -1;
        }
    } else {
        if (cgrtos_snprintf(joined, sizeof(joined), "%s/%s", s->cwd, user) < 0) {
            return -1;
        }
    }
    if (cli_path_normalize(joined, abs, abs_sz) != 0) {
        if (!quiet) {
            cgrtos_printf("fs: path traversal/invalid '%s'\n", user);
        }
        return -1;
    }
    return 0;
}

int cli_path_getcwd(char *out, size_t out_sz)
{
    cli_path_sess_t *s = path_sess_get();
    if (!s || !out || out_sz < 2) {
        return -1;
    }
    strncpy(out, s->cwd, out_sz);
    out[out_sz - 1] = 0;
    return 0;
}

int cli_path_setcwd(const char *abs)
{
    cli_path_sess_t *s = path_sess_get();
    if (!s || !abs || abs[0] != '/') {
        return -1;
    }
    strncpy(s->cwd, abs, sizeof(s->cwd));
    s->cwd[sizeof(s->cwd) - 1] = 0;
    return 0;
}

/**
 * @brief 定位行内光标处的路径 token [start, end)
 * @internal
 */
static void path_token_span(const char *line, int len, int cursor, int *start, int *end)
{
    int i = cursor;
    if (i > len) {
        i = len;
    }
    while (i > 0 && line[i - 1] != ' ' && line[i - 1] != '\t') {
        i--;
    }
    *start = i;
    i = cursor;
    while (i < len && line[i] != ' ' && line[i] != '\t') {
        i++;
    }
    *end = i;
}

/**
 * @brief 判断 name 是否以 prefix 开头
 * @internal
 */
static int path_prefix_match(const char *name, const char *prefix)
{
    while (*prefix) {
        if (*name++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief 计算多串最长公共前缀长度
 * @internal
 */
static int path_lcp_len(char names[][CGRTOS_FS_MAX_NAME], int n)
{
    int L = 0;
    if (n <= 0) {
        return 0;
    }
    for (;;) {
        char c = names[0][L];
        if (c == 0) {
            return L;
        }
        for (int i = 1; i < n; i++) {
            if (names[i][L] != c) {
                return L;
            }
        }
        L++;
        if (L >= CGRTOS_FS_MAX_NAME - 1) {
            return L;
        }
    }
}

int cli_path_complete(char *line, int line_cap, int *len, int *cursor, int list_all)
{
    int tok_s = 0, tok_e = 0;
    char token[CLI_FS_PATH_MAX];
    char abs[CLI_FS_PATH_MAX];
    char dir[CLI_FS_PATH_MAX];
    char prefix[CGRTOS_FS_MAX_NAME];
    char (*matches)[CGRTOS_FS_MAX_NAME] = 0;
    uint8_t *isdir = 0;
    int nmatch = 0;
    cgrtos_dir_t *dp = 0;
    int rc = 0;
    int slash_at_end = 0;

    if (!line || !len || !cursor || line_cap < 2) {
        return -1;
    }
    if (*len < 0 || *len >= line_cap || *cursor < 0 || *cursor > *len) {
        cgrtos_printf("complete: bad cursor/len\n");
        return -1;
    }

    path_token_span(line, *len, *cursor, &tok_s, &tok_e);
    if (tok_e - tok_s >= (int)sizeof(token)) {
        cgrtos_printf("complete: token too long\n");
        return -1;
    }
    memcpy(token, line + tok_s, (size_t)(tok_e - tok_s));
    token[tok_e - tok_s] = 0;

    /* 空 token：补当前目录下全部；非路径命令参数也允许补文件名 */
    slash_at_end = (token[0] != 0 && token[strlen(token) - 1] == '/');

    /* 拆 dir + prefix */
    {
        const char *slash = strrchr(token, '/');
        char parent_tok[CLI_FS_PATH_MAX];
        if (!slash) {
            /* 相对名：目录 = CWD，前缀 = token */
            if (cli_path_getcwd(dir, sizeof(dir)) != 0) {
                cgrtos_printf("complete: no session\n");
                return -1;
            }
            strncpy(prefix, token, sizeof(prefix));
            prefix[sizeof(prefix) - 1] = 0;
        } else if (slash == token && token[1] == 0) {
            /* "/" */
            dir[0] = '/';
            dir[1] = 0;
            prefix[0] = 0;
        } else {
            size_t plen = (size_t)(slash - token);
            if (plen >= sizeof(parent_tok)) {
                cgrtos_printf("complete: path too long\n");
                return -1;
            }
            memcpy(parent_tok, token, plen);
            parent_tok[plen] = 0;
            if (parent_tok[0] == 0) {
                /* "/foo" 前缀情况：parent is empty before slash → "/" */
                dir[0] = '/';
                dir[1] = 0;
            } else {
                if (cli_path_resolve(parent_tok, abs, sizeof(abs), 1) != 0) {
                    cgrtos_uart_putc('\a');
                    return 0;
                }
                strncpy(dir, abs, sizeof(dir));
                dir[sizeof(dir) - 1] = 0;
            }
            strncpy(prefix, slash + 1, sizeof(prefix));
            prefix[sizeof(prefix) - 1] = 0;
        }
    }
    (void)slash_at_end;

    /* 验证 dir 是目录（vfs_stat 持 g_fs_mtx 短暂） */
    {
        cgrtos_stat_t st;
        if (vfs_stat(dir, &st) != 0 || (st.mode & CGRTOS_S_IFDIR) == 0) {
            cgrtos_printf("complete: not a directory '%s'\n", dir);
            return -1;
        }
    }

    /* 先分配匹配表（禁止在持 FS 锁时 malloc —— 此处尚未持锁） */
    matches = (char (*)[CGRTOS_FS_MAX_NAME])cgrtos_malloc(
        (unsigned long)(CLI_PATH_MATCH_MAX * CGRTOS_FS_MAX_NAME));
    isdir = (uint8_t *)cgrtos_malloc(CLI_PATH_MATCH_MAX);
    if (!matches || !isdir) {
        cgrtos_printf("complete: out of memory\n");
        cgrtos_free(matches);
        cgrtos_free(isdir);
        return -1;
    }
    memset(matches, 0, (size_t)(CLI_PATH_MATCH_MAX * CGRTOS_FS_MAX_NAME));
    memset(isdir, 0, CLI_PATH_MATCH_MAX);

    dp = vfs_opendir(dir);
    if (!dp) {
        cgrtos_printf("complete: opendir failed '%s'\n", dir);
        cgrtos_free(matches);
        cgrtos_free(isdir);
        return -1;
    }

    /* readdir 循环：仅收集，不 printf（缩短持锁窗口由 vfs 内部管理） */
    for (;;) {
        cgrtos_dirent_t ent;
        int r = vfs_readdir(dp, &ent);
        if (r < 0) {
            cgrtos_printf("complete: readdir error\n");
            vfs_closedir(dp);
            cgrtos_free(matches);
            cgrtos_free(isdir);
            return -1;
        }
        if (r == 0) {
            break;
        }
        if (!path_prefix_match(ent.name, prefix)) {
            continue;
        }
        if (nmatch >= CLI_PATH_MATCH_MAX) {
            cgrtos_printf("complete: too many matches (>%d)\n", CLI_PATH_MATCH_MAX);
            vfs_closedir(dp);
            cgrtos_free(matches);
            cgrtos_free(isdir);
            return -1;
        }
        strncpy(matches[nmatch], ent.name, CGRTOS_FS_MAX_NAME);
        matches[nmatch][CGRTOS_FS_MAX_NAME - 1] = 0;
        isdir[nmatch] = (ent.mode & CGRTOS_S_IFDIR) ? 1 : 0;
        nmatch++;
    }
    if (vfs_closedir(dp) != 0) {
        /* 非致命：匹配表仍可用 */
    }

    if (nmatch == 0) {
        cgrtos_uart_putc('\a');
        cgrtos_free(matches);
        cgrtos_free(isdir);
        return 0;
    }

    if (list_all) {
        cgrtos_uart_putc('\n');
        for (int i = 0; i < nmatch; i++) {
            cgrtos_printf("  %s%s\n", matches[i], isdir[i] ? "/" : "");
        }
        cgrtos_free(matches);
        cgrtos_free(isdir);
        return 0; /* 行未改；调用方重绘 */
    }

    /* 计算要写入 token 的新 basename（LCP 或唯一名） */
    {
        char insert[CGRTOS_FS_MAX_NAME + 2];
        int ilen;
        int new_tok_len;
        char new_tok[CLI_FS_PATH_MAX];
        int delta;
        int i;

        if (nmatch == 1) {
            ilen = (int)strlen(matches[0]);
            if (ilen + (isdir[0] ? 1 : 0) + 1 > (int)sizeof(insert)) {
                cgrtos_printf("complete: name too long\n");
                cgrtos_free(matches);
                cgrtos_free(isdir);
                return -1;
            }
            memcpy(insert, matches[0], (size_t)ilen);
            if (isdir[0]) {
                insert[ilen++] = '/';
            }
            insert[ilen] = 0;
        } else {
            ilen = path_lcp_len(matches, nmatch);
            if (ilen <= (int)strlen(prefix)) {
                /* 无法进一步补全 */
                cgrtos_uart_putc('\a');
                cgrtos_free(matches);
                cgrtos_free(isdir);
                return 0;
            }
            memcpy(insert, matches[0], (size_t)ilen);
            insert[ilen] = 0;
        }

        /* 重建 token：dir 部分 + insert */
        {
            const char *slash = strrchr(token, '/');
            if (!slash) {
                strncpy(new_tok, insert, sizeof(new_tok));
                new_tok[sizeof(new_tok) - 1] = 0;
            } else {
                size_t keep = (size_t)(slash - token + 1);
                if (keep + strlen(insert) + 1 > sizeof(new_tok)) {
                    cgrtos_printf("complete: result too long\n");
                    cgrtos_free(matches);
                    cgrtos_free(isdir);
                    return -1;
                }
                memcpy(new_tok, token, keep);
                new_tok[keep] = 0;
                strncat(new_tok, insert, sizeof(new_tok) - keep - 1);
            }
        }
        new_tok_len = (int)strlen(new_tok);
        delta = new_tok_len - (tok_e - tok_s);
        if (*len + delta >= line_cap) {
            cgrtos_printf("complete: line overflow\n");
            cgrtos_free(matches);
            cgrtos_free(isdir);
            return -1;
        }
        /* 移动 token 之后的内容 */
        if (delta > 0) {
            for (i = *len - 1; i >= tok_e; i--) {
                line[i + delta] = line[i];
            }
        } else if (delta < 0) {
            for (i = tok_e; i < *len; i++) {
                line[i + delta] = line[i];
            }
        }
        memcpy(line + tok_s, new_tok, (size_t)new_tok_len);
        *len += delta;
        line[*len] = 0;
        *cursor = tok_s + new_tok_len;
        rc = 1;
    }

    cgrtos_free(matches);
    cgrtos_free(isdir);
    return rc;
}

#endif /* CONFIG_CLI_FS */
