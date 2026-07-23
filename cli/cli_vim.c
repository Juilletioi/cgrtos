/**
 * @file cli_vim.c
 * @brief POSIX vi 核心编辑器（Normal/Insert/Visual/Cmdline）
 * @author Cong Zhou / Juilletioi
 * @version 5.1.0
 * @date 2026-07-23
 * @copyright CG-RTOS
 *
 * @details
 * ## 并发保护（必须遵守）
 * | 数据 | 保护 |
 * |------|------|
 * | text/undo/clip/状态 | 仅 cli 任务，无锁 |
 * | 文件 load/save | vfs_* → g_fs_mtx；持锁禁止 printf/getc/yield/malloc |
 * | 堆 | cgrtos_malloc → g_klock；先 malloc 再 open/read |
 * | 控制台 | g_console_lock；禁止持 FS 锁时 printf |
 *
 * ## 死锁 / 抢占 / ISR
 * 1. 持 g_fs_mtx + 阻塞 UART → 其他任务 FS 饿死：禁止。
 * 2. 持 g_fs_mtx + malloc → 锁顺序混乱：禁止。
 * 3. 与 fs_abort_poll 同任务互斥，无并行吞字节竞态。
 * 4. 高优先级任务可抢占 cli，但不得访问本编辑器状态。
 * 5. 全部 API ❌ ISR。
 *
 * 完整键位与竞态说明见 docs/MODULE_CLI_VIM.md。
 */
#include "cli_vim.h"

#if CONFIG_CLI_VIM && CONFIG_CLI_FS

#include "cli_path.h"
#include "../kernel/vfs.h"
#include <string.h>

#ifndef CGRTOS_VIM_UNDO_MAX
#define CGRTOS_VIM_UNDO_MAX 64
#endif
#ifndef CGRTOS_VIM_CLIP_MAX
#define CGRTOS_VIM_CLIP_MAX 4096
#endif
#ifndef CGRTOS_VIM_PAT_MAX
#define CGRTOS_VIM_PAT_MAX 64
#endif
#ifndef CGRTOS_VIM_CMDLINE_MAX
#define CGRTOS_VIM_CMDLINE_MAX 96
#endif

#define VIM_ROWS       22
#define VIM_COLS       80
#define VIM_MSG_MAX    80

typedef enum {
    VIM_NORMAL = 0,
    VIM_INSERT,
    VIM_VISUAL,
    VIM_CMDLINE
} vim_mode_t;

typedef enum {
    U_INSERT = 1,
    U_DELETE
} vim_undo_kind_t;

typedef struct {
    uint8_t        used;
    vim_undo_kind_t kind;
    size_t         pos;     /* 操作起始偏移 */
    size_t         len;     /* 文本长度 */
    char          *data;    /* 被删或插入的副本（heap） */
} vim_undo_t;

typedef struct {
    char          *text;
    size_t         len;
    size_t         cap;
    size_t        *loff;    /* 每行起始偏移；loff[nlines]=len */
    int            nlines;
    size_t         cur;     /* 字节光标 */
    size_t         vanchor; /* Visual 锚点 */
    int            toprow;  /* 视口首行 */
    vim_mode_t     mode;
    int            modified;
    int            show_nu;
    int            count;   /* 数字前缀 */
    int            pending; /* 待操作符：'d','y',0 */
    char           filename[CLI_FS_PATH_MAX];
    char           msg[VIM_MSG_MAX];
    char           cmdline[CGRTOS_VIM_CMDLINE_MAX];
    int            cmdlen;
    int            cmdkind; /* ':' '/' '?' */
    char           pattern[CGRTOS_VIM_PAT_MAX];
    int            search_fwd;
    char          *clip;
    size_t         clip_len;
    int            clip_linewise;
    vim_undo_t     undo[CGRTOS_VIM_UNDO_MAX];
    int            undo_sp;
    int            undo_count;
    /* 上次改动（.） */
    int            last_op; /* 0=无 1=ins 2=del_char 3=del_line 4=paste */
    char          *last_ins;
    size_t         last_ins_len;
    int            quit;
} vim_t;

/** @brief 编辑器全局状态：cli 任务独占，无锁 */
static vim_t g_vim;

static void vim_set_msg(const char *s)
{
    if (!s) {
        g_vim.msg[0] = 0;
        return;
    }
    strncpy(g_vim.msg, s, VIM_MSG_MAX - 1);
    g_vim.msg[VIM_MSG_MAX - 1] = 0;
}

static int vim_rebuild_lines(void)
{
    size_t i;
    int n = 1;
    size_t *nl;
    for (i = 0; i < g_vim.len; i++) {
        if (g_vim.text[i] == '\n') {
            n++;
        }
    }
    if (g_vim.len > 0 && g_vim.text[g_vim.len - 1] == '\n') {
        /* trailing newline → empty last line still counts as line in vi */
    }
    nl = (size_t *)cgrtos_malloc((unsigned long)(sizeof(size_t) * (unsigned)(n + 1)));
    if (!nl) {
        vim_set_msg("E: nomem lines");
        return -1;
    }
    n = 0;
    nl[n++] = 0;
    for (i = 0; i < g_vim.len; i++) {
        if (g_vim.text[i] == '\n') {
            nl[n++] = i + 1;
        }
    }
    nl[n] = g_vim.len;
    cgrtos_free(g_vim.loff);
    g_vim.loff = nl;
    g_vim.nlines = n;
    return 0;
}

static void vim_off_to_rc(size_t off, int *row, int *col)
{
    int r;
    if (!g_vim.loff || g_vim.nlines <= 0) {
        *row = 0;
        *col = 0;
        return;
    }
    if (off > g_vim.len) {
        off = g_vim.len;
    }
    for (r = 0; r < g_vim.nlines; r++) {
        if (off < g_vim.loff[r + 1] || r == g_vim.nlines - 1) {
            if (off >= g_vim.loff[r + 1] && r < g_vim.nlines - 1) {
                continue;
            }
            *row = r;
            *col = (int)(off - g_vim.loff[r]);
            {
                size_t line_end = g_vim.loff[r + 1];
                if (line_end > g_vim.loff[r] && g_vim.text[line_end - 1] == '\n') {
                    line_end--;
                }
                if (off > line_end) {
                    *col = (int)(line_end - g_vim.loff[r]);
                }
            }
            return;
        }
    }
    *row = g_vim.nlines - 1;
    *col = 0;
}

static size_t vim_rc_to_off(int row, int col)
{
    size_t start, end, maxcol;
    if (!g_vim.loff || g_vim.nlines <= 0) {
        return 0;
    }
    if (row < 0) {
        row = 0;
    }
    if (row >= g_vim.nlines) {
        row = g_vim.nlines - 1;
    }
    start = g_vim.loff[row];
    end = g_vim.loff[row + 1];
    if (end > start && g_vim.text[end - 1] == '\n') {
        end--;
    }
    maxcol = end - start;
    if (col < 0) {
        col = 0;
    }
    if ((size_t)col > maxcol) {
        col = (int)maxcol;
    }
    return start + (size_t)col;
}

static int vim_ensure_cap(size_t need)
{
    char *nbuf;
    size_t ncap;
    if (need <= g_vim.cap) {
        return 0;
    }
    ncap = g_vim.cap ? g_vim.cap * 2 : 256;
    while (ncap < need) {
        ncap *= 2;
    }
    if (ncap > CGRTOS_FS_MAX_FILE_BYTES + 1) {
        ncap = CGRTOS_FS_MAX_FILE_BYTES + 1;
    }
    if (need > ncap) {
        vim_set_msg("E: file too large");
        return -1;
    }
    /* malloc 在无 FS 锁下 */
    nbuf = (char *)cgrtos_malloc(ncap);
    if (!nbuf) {
        vim_set_msg("E: out of memory");
        return -1;
    }
    if (g_vim.text && g_vim.len) {
        memcpy(nbuf, g_vim.text, g_vim.len);
    }
    cgrtos_free(g_vim.text);
    g_vim.text = nbuf;
    g_vim.cap = ncap;
    return 0;
}

static void vim_undo_clear(void)
{
    for (int i = 0; i < CGRTOS_VIM_UNDO_MAX; i++) {
        cgrtos_free(g_vim.undo[i].data);
        g_vim.undo[i].data = 0;
        g_vim.undo[i].used = 0;
    }
    g_vim.undo_sp = 0;
    g_vim.undo_count = 0;
}

static int vim_undo_push(vim_undo_kind_t kind, size_t pos, const char *data, size_t len)
{
    vim_undo_t *u;
    char *copy;
    if (len > 0 && !data) {
        return -1;
    }
    copy = 0;
    if (len > 0) {
        copy = (char *)cgrtos_malloc(len + 1);
        if (!copy) {
            vim_set_msg("E: undo nomem");
            return -1;
        }
        memcpy(copy, data, len);
        copy[len] = 0;
    }
    u = &g_vim.undo[g_vim.undo_sp];
    cgrtos_free(u->data);
    u->used = 1;
    u->kind = kind;
    u->pos = pos;
    u->len = len;
    u->data = copy;
    g_vim.undo_sp = (g_vim.undo_sp + 1) % CGRTOS_VIM_UNDO_MAX;
    if (g_vim.undo_count < CGRTOS_VIM_UNDO_MAX) {
        g_vim.undo_count++;
    }
    return 0;
}

static int vim_buf_delete(size_t pos, size_t n, int record_undo)
{
    if (pos > g_vim.len || n == 0) {
        return 0;
    }
    if (pos + n > g_vim.len) {
        n = g_vim.len - pos;
    }
    if (record_undo) {
        if (vim_undo_push(U_DELETE, pos, g_vim.text + pos, n) != 0) {
            return -1;
        }
    }
    memmove(g_vim.text + pos, g_vim.text + pos + n, g_vim.len - pos - n);
    g_vim.len -= n;
    g_vim.modified = 1;
    if (g_vim.cur > g_vim.len) {
        g_vim.cur = g_vim.len;
    } else if (g_vim.cur > pos) {
        if (g_vim.cur >= pos + n) {
            g_vim.cur -= n;
        } else {
            g_vim.cur = pos;
        }
    }
    return vim_rebuild_lines();
}

static int vim_buf_insert(size_t pos, const char *data, size_t n, int record_undo)
{
    if (!data && n) {
        return -1;
    }
    if (pos > g_vim.len) {
        pos = g_vim.len;
    }
    if (g_vim.len + n > CGRTOS_FS_MAX_FILE_BYTES) {
        vim_set_msg("E: max file size");
        return -1;
    }
    if (vim_ensure_cap(g_vim.len + n + 1) != 0) {
        return -1;
    }
    if (record_undo) {
        if (vim_undo_push(U_INSERT, pos, data, n) != 0) {
            return -1;
        }
    }
    memmove(g_vim.text + pos + n, g_vim.text + pos, g_vim.len - pos);
    if (n) {
        memcpy(g_vim.text + pos, data, n);
    }
    g_vim.len += n;
    g_vim.cur = pos + n;
    g_vim.modified = 1;
    return vim_rebuild_lines();
}

static int vim_do_undo(void)
{
    vim_undo_t *u;
    int idx;
    if (g_vim.undo_count <= 0) {
        vim_set_msg("Already at oldest change");
        return -1;
    }
    idx = g_vim.undo_sp - 1;
    if (idx < 0) {
        idx += CGRTOS_VIM_UNDO_MAX;
    }
    u = &g_vim.undo[idx];
    if (!u->used) {
        vim_set_msg("Already at oldest change");
        return -1;
    }
    if (u->kind == U_INSERT) {
        /* 撤销插入 = 删除 */
        if (vim_buf_delete(u->pos, u->len, 0) != 0) {
            return -1;
        }
        g_vim.cur = u->pos;
    } else {
        /* 撤销删除 = 插回 */
        if (vim_buf_insert(u->pos, u->data, u->len, 0) != 0) {
            return -1;
        }
        g_vim.cur = u->pos + u->len;
    }
    cgrtos_free(u->data);
    u->data = 0;
    u->used = 0;
    g_vim.undo_sp = idx;
    g_vim.undo_count--;
    vim_set_msg("Undo");
    return 0;
}

static int vim_clip_set(const char *data, size_t n, int linewise)
{
    char *p;
    if (n > CGRTOS_VIM_CLIP_MAX) {
        vim_set_msg("E: clipboard overflow");
        return -1;
    }
    p = (char *)cgrtos_malloc(n + 1);
    if (!p && n > 0) {
        vim_set_msg("E: clipboard nomem");
        return -1;
    }
    if (n && data) {
        memcpy(p, data, n);
    }
    if (p) {
        p[n] = 0;
    }
    cgrtos_free(g_vim.clip);
    g_vim.clip = p;
    g_vim.clip_len = n;
    g_vim.clip_linewise = linewise;
    return 0;
}

static void vim_scroll_into_view(void)
{
    int row, col;
    vim_off_to_rc(g_vim.cur, &row, &col);
    (void)col;
    if (row < g_vim.toprow) {
        g_vim.toprow = row;
    }
    if (row >= g_vim.toprow + VIM_ROWS) {
        g_vim.toprow = row - VIM_ROWS + 1;
    }
    if (g_vim.toprow < 0) {
        g_vim.toprow = 0;
    }
}

static void vim_draw(void)
{
    int row, col;
    int r;
    int gut = g_vim.show_nu ? 6 : 0;

    vim_scroll_into_view();
    vim_off_to_rc(g_vim.cur, &row, &col);

    cgrtos_uart_puts("\033[2J\033[H");
    for (r = 0; r < VIM_ROWS; r++) {
        int lr = g_vim.toprow + r;
        cgrtos_printf("\033[%d;1H", r + 1);
        if (lr >= g_vim.nlines) {
            cgrtos_uart_puts("~");
            cgrtos_uart_puts("\033[K");
            continue;
        }
        if (g_vim.show_nu) {
            cgrtos_printf("%4d  ", lr + 1);
        }
        {
            size_t a = g_vim.loff[lr];
            size_t b = g_vim.loff[lr + 1];
            int c = 0;
            if (b > a && g_vim.text[b - 1] == '\n') {
                b--;
            }
            while (a < b && c < VIM_COLS - gut) {
                char ch = g_vim.text[a++];
                if (ch == '\t') {
                    cgrtos_uart_putc(' ');
                } else if (ch >= 32 && ch < 127) {
                    cgrtos_uart_putc(ch);
                } else {
                    cgrtos_uart_putc('?');
                }
                c++;
            }
        }
        cgrtos_uart_puts("\033[K");
    }
    /* status */
    cgrtos_printf("\033[%d;1H", VIM_ROWS + 1);
    {
        const char *modename = "NORMAL";
        if (g_vim.mode == VIM_INSERT) {
            modename = "INSERT";
        } else if (g_vim.mode == VIM_VISUAL) {
            modename = "VISUAL";
        } else if (g_vim.mode == VIM_CMDLINE) {
            modename = "CMDLINE";
        }
        cgrtos_printf("%s%s  %s  %d,%d\033[K",
                      g_vim.filename[0] ? g_vim.filename : "[No Name]",
                      g_vim.modified ? " [+]" : "",
                      modename, row + 1, col + 1);
    }
    cgrtos_printf("\033[%d;1H", VIM_ROWS + 2);
    if (g_vim.mode == VIM_CMDLINE) {
        cgrtos_printf("%c%s\033[K", (char)g_vim.cmdkind, g_vim.cmdline);
    } else if (g_vim.msg[0]) {
        cgrtos_printf("%s\033[K", g_vim.msg);
    } else {
        cgrtos_uart_puts("\033[K");
    }
    /* cursor */
    {
        int scr_r = row - g_vim.toprow + 1;
        int scr_c = col + 1 + gut;
        if (scr_r < 1) {
            scr_r = 1;
        }
        if (scr_r > VIM_ROWS) {
            scr_r = VIM_ROWS;
        }
        if (g_vim.mode == VIM_CMDLINE) {
            cgrtos_printf("\033[%d;%dH", VIM_ROWS + 2, g_vim.cmdlen + 2);
        } else {
            cgrtos_printf("\033[%d;%dH", scr_r, scr_c);
        }
    }
}

static int vim_load(const char *abs)
{
    cgrtos_stat_t st;
    int fd = -1;
    int created = 0;
    char *buf = 0;

    memset(&g_vim, 0, sizeof(g_vim));
    g_vim.search_fwd = 1;
    strncpy(g_vim.filename, abs, sizeof(g_vim.filename) - 1);

    if (vfs_stat(abs, &st) == 0) {
        if (st.mode & CGRTOS_S_IFDIR) {
            vim_set_msg("E: is a directory");
            return -1;
        }
        if (st.size > CGRTOS_FS_MAX_FILE_BYTES) {
            vim_set_msg("E: file too large");
            return -1;
        }
        /* 先 malloc 再 open（锁顺序） */
        buf = (char *)cgrtos_malloc(st.size + 1);
        if (!buf && st.size > 0) {
            vim_set_msg("E: out of memory");
            return -1;
        }
        fd = vfs_open(abs, CGRTOS_O_RDWR);
        if (fd < 0) {
            cgrtos_free(buf);
            vim_set_msg("E: cannot open");
            return -1;
        }
        if (st.size > 0) {
            int n = vfs_read(fd, buf, st.size);
            if (n < 0 || (uint32_t)n != st.size) {
                vfs_close(fd);
                cgrtos_free(buf);
                vim_set_msg("E: read failed");
                return -1;
            }
        }
        vfs_close(fd);
        g_vim.text = buf;
        g_vim.len = st.size;
        g_vim.cap = st.size + 1;
        if (g_vim.text) {
            g_vim.text[g_vim.len] = 0;
        }
    } else {
        /* 新文件：创建空 */
        fd = vfs_open(abs, CGRTOS_O_CREAT | CGRTOS_O_RDWR);
        if (fd < 0) {
            vim_set_msg("E: cannot create");
            return -1;
        }
        vfs_close(fd);
        created = 1;
        g_vim.text = (char *)cgrtos_malloc(256);
        if (!g_vim.text) {
            vim_set_msg("E: out of memory");
            return -1;
        }
        g_vim.cap = 256;
        g_vim.len = 0;
        g_vim.text[0] = 0;
        g_vim.modified = 0;
    }
    if (vim_rebuild_lines() != 0) {
        cgrtos_free(g_vim.text);
        g_vim.text = 0;
        return -1;
    }
    g_vim.cur = 0;
    (void)created;
    return 0;
}

static int vim_save(const char *path)
{
    const char *p = path && path[0] ? path : g_vim.filename;
    int fd;
    int n;
    char abs[CLI_FS_PATH_MAX];

    if (!p || !p[0]) {
        vim_set_msg("E: no file name");
        return -1;
    }
    if (cli_path_resolve(p, abs, sizeof(abs), 0) != 0) {
        return -1;
    }
    /* 不在持锁时准备路径；open/write/close 由 vfs 持锁 */
    fd = vfs_open(abs, CGRTOS_O_WRONLY | CGRTOS_O_CREAT | CGRTOS_O_TRUNC);
    if (fd < 0) {
        vim_set_msg("E: write open failed");
        return -1;
    }
    n = 0;
    if (g_vim.len > 0) {
        n = vfs_write(fd, g_vim.text, g_vim.len);
    }
    if (vfs_close(fd) != 0) {
        vim_set_msg("E: close failed");
        return -1;
    }
    if (g_vim.len > 0 && (n < 0 || (size_t)n != g_vim.len)) {
        vim_set_msg("E: write failed");
        return -1;
    }
    strncpy(g_vim.filename, abs, sizeof(g_vim.filename) - 1);
    g_vim.modified = 0;
    vim_set_msg("written");
    return 0;
}

static void vim_free_all(void)
{
    vim_undo_clear();
    cgrtos_free(g_vim.text);
    cgrtos_free(g_vim.loff);
    cgrtos_free(g_vim.clip);
    cgrtos_free(g_vim.last_ins);
    memset(&g_vim, 0, sizeof(g_vim));
}

static void vim_move_left(int n)
{
    while (n-- > 0 && g_vim.cur > 0) {
        g_vim.cur--;
    }
}

static void vim_move_right(int n)
{
    while (n-- > 0 && g_vim.cur < g_vim.len) {
        if (g_vim.text[g_vim.cur] == '\n') {
            break;
        }
        g_vim.cur++;
    }
}

static void vim_move_down(int n)
{
    int row, col;
    vim_off_to_rc(g_vim.cur, &row, &col);
    row += n;
    if (row >= g_vim.nlines) {
        row = g_vim.nlines - 1;
    }
    g_vim.cur = vim_rc_to_off(row, col);
}

static void vim_move_up(int n)
{
    int row, col;
    vim_off_to_rc(g_vim.cur, &row, &col);
    row -= n;
    if (row < 0) {
        row = 0;
    }
    g_vim.cur = vim_rc_to_off(row, col);
}

static int vim_is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static void vim_word_fwd(int n)
{
    while (n-- > 0) {
        if (g_vim.cur >= g_vim.len) {
            return;
        }
        if (vim_is_word(g_vim.text[g_vim.cur])) {
            while (g_vim.cur < g_vim.len && vim_is_word(g_vim.text[g_vim.cur])) {
                g_vim.cur++;
            }
        } else if (g_vim.text[g_vim.cur] != ' ' && g_vim.text[g_vim.cur] != '\t' &&
                   g_vim.text[g_vim.cur] != '\n') {
            while (g_vim.cur < g_vim.len && !vim_is_word(g_vim.text[g_vim.cur]) &&
                   g_vim.text[g_vim.cur] != ' ' && g_vim.text[g_vim.cur] != '\t' &&
                   g_vim.text[g_vim.cur] != '\n') {
                g_vim.cur++;
            }
        }
        while (g_vim.cur < g_vim.len &&
               (g_vim.text[g_vim.cur] == ' ' || g_vim.text[g_vim.cur] == '\t' ||
                g_vim.text[g_vim.cur] == '\n')) {
            g_vim.cur++;
        }
    }
}

static void vim_word_end(int n)
{
    while (n-- > 0) {
        if (g_vim.cur + 1 < g_vim.len) {
            g_vim.cur++;
        }
        while (g_vim.cur < g_vim.len &&
               (g_vim.text[g_vim.cur] == ' ' || g_vim.text[g_vim.cur] == '\t' ||
                g_vim.text[g_vim.cur] == '\n')) {
            g_vim.cur++;
        }
        if (g_vim.cur >= g_vim.len) {
            return;
        }
        if (vim_is_word(g_vim.text[g_vim.cur])) {
            while (g_vim.cur + 1 < g_vim.len && vim_is_word(g_vim.text[g_vim.cur + 1])) {
                g_vim.cur++;
            }
        } else {
            while (g_vim.cur + 1 < g_vim.len && !vim_is_word(g_vim.text[g_vim.cur + 1]) &&
                   g_vim.text[g_vim.cur + 1] != ' ' && g_vim.text[g_vim.cur + 1] != '\t' &&
                   g_vim.text[g_vim.cur + 1] != '\n') {
                g_vim.cur++;
            }
        }
    }
}

static void vim_word_back(int n)
{
    while (n-- > 0) {
        if (g_vim.cur == 0) {
            return;
        }
        g_vim.cur--;
        while (g_vim.cur > 0 &&
               (g_vim.text[g_vim.cur] == ' ' || g_vim.text[g_vim.cur] == '\t' ||
                g_vim.text[g_vim.cur] == '\n')) {
            g_vim.cur--;
        }
        if (vim_is_word(g_vim.text[g_vim.cur])) {
            while (g_vim.cur > 0 && vim_is_word(g_vim.text[g_vim.cur - 1])) {
                g_vim.cur--;
            }
        } else {
            while (g_vim.cur > 0 && !vim_is_word(g_vim.text[g_vim.cur - 1]) &&
                   g_vim.text[g_vim.cur - 1] != ' ' && g_vim.text[g_vim.cur - 1] != '\t' &&
                   g_vim.text[g_vim.cur - 1] != '\n') {
                g_vim.cur--;
            }
        }
    }
}

static void vim_line_begin(void)
{
    int row, col;
    vim_off_to_rc(g_vim.cur, &row, &col);
    (void)col;
    g_vim.cur = g_vim.loff[row];
}

static void vim_line_first_nonblank(void)
{
    int row, col;
    size_t p, end;
    vim_off_to_rc(g_vim.cur, &row, &col);
    (void)col;
    p = g_vim.loff[row];
    end = g_vim.loff[row + 1];
    if (end > p && g_vim.text[end - 1] == '\n') {
        end--;
    }
    while (p < end && (g_vim.text[p] == ' ' || g_vim.text[p] == '\t')) {
        p++;
    }
    g_vim.cur = p;
}

static void vim_line_end(void)
{
    int row, col;
    size_t end;
    vim_off_to_rc(g_vim.cur, &row, &col);
    (void)col;
    end = g_vim.loff[row + 1];
    if (end > g_vim.loff[row] && g_vim.text[end - 1] == '\n') {
        end--;
    }
    g_vim.cur = end;
}

static int vim_delete_lines(int n)
{
    int row, col;
    size_t a, b;
    vim_off_to_rc(g_vim.cur, &row, &col);
    (void)col;
    if (n < 1) {
        n = 1;
    }
    if (row + n > g_vim.nlines) {
        n = g_vim.nlines - row;
    }
    a = g_vim.loff[row];
    b = g_vim.loff[row + n];
    if (vim_clip_set(g_vim.text + a, b - a, 1) != 0) {
        return -1;
    }
    if (vim_buf_delete(a, b - a, 1) != 0) {
        return -1;
    }
    g_vim.cur = a;
    if (g_vim.cur > g_vim.len) {
        g_vim.cur = g_vim.len;
    }
    g_vim.last_op = 3;
    return 0;
}

static int vim_yank_lines(int n)
{
    int row, col;
    size_t a, b;
    vim_off_to_rc(g_vim.cur, &row, &col);
    (void)col;
    if (n < 1) {
        n = 1;
    }
    if (row + n > g_vim.nlines) {
        n = g_vim.nlines - row;
    }
    a = g_vim.loff[row];
    b = g_vim.loff[row + n];
    return vim_clip_set(g_vim.text + a, b - a, 1);
}

static int vim_paste(int after)
{
    size_t pos = g_vim.cur;
    if (!g_vim.clip || g_vim.clip_len == 0) {
        vim_set_msg("E: empty register");
        return -1;
    }
    if (g_vim.clip_linewise) {
        int row, col;
        vim_off_to_rc(g_vim.cur, &row, &col);
        (void)col;
        if (after) {
            pos = g_vim.loff[row + 1];
        } else {
            pos = g_vim.loff[row];
        }
    } else if (after && pos < g_vim.len && g_vim.text[pos] != '\n') {
        pos++;
    }
    if (vim_buf_insert(pos, g_vim.clip, g_vim.clip_len, 1) != 0) {
        return -1;
    }
    g_vim.last_op = 4;
    return 0;
}

static int vim_search(int fwd, int wrap_msg)
{
    size_t i;
    size_t plen;
    if (!g_vim.pattern[0]) {
        vim_set_msg("E: no previous pattern");
        return -1;
    }
    plen = strlen(g_vim.pattern);
    if (fwd) {
        i = g_vim.cur + 1;
        while (i + plen <= g_vim.len) {
            if (memcmp(g_vim.text + i, g_vim.pattern, plen) == 0) {
                g_vim.cur = i;
                return 0;
            }
            i++;
        }
        /* wrap */
        i = 0;
        while (i <= g_vim.cur && i + plen <= g_vim.len) {
            if (memcmp(g_vim.text + i, g_vim.pattern, plen) == 0) {
                g_vim.cur = i;
                if (wrap_msg) {
                    vim_set_msg("search hit BOTTOM, continuing at TOP");
                }
                return 0;
            }
            i++;
        }
    } else {
        if (g_vim.cur == 0) {
            i = g_vim.len;
        } else {
            i = g_vim.cur - 1;
        }
        for (;;) {
            if (i + plen <= g_vim.len &&
                memcmp(g_vim.text + i, g_vim.pattern, plen) == 0) {
                g_vim.cur = i;
                return 0;
            }
            if (i == 0) {
                break;
            }
            i--;
        }
        i = g_vim.len > plen ? g_vim.len - plen : 0;
        while (i > g_vim.cur) {
            if (memcmp(g_vim.text + i, g_vim.pattern, plen) == 0) {
                g_vim.cur = i;
                if (wrap_msg) {
                    vim_set_msg("search hit TOP, continuing at BOTTOM");
                }
                return 0;
            }
            if (i == 0) {
                break;
            }
            i--;
        }
    }
    vim_set_msg("Pattern not found");
    return -1;
}

static int vim_ex_run(void)
{
    char *p = g_vim.cmdline;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == 0) {
        return 0;
    }
    if (p[0] == 'q' && p[1] == '!' && p[2] == 0) {
        g_vim.quit = 1;
        return 0;
    }
    if ((p[0] == 'q' && p[1] == 0) || (p[0] == 'q' && p[1] == 'u' && p[2] == 'i' && p[3] == 't' && p[4] == 0)) {
        if (g_vim.modified) {
            vim_set_msg("E: No write since last change (:q! overrides)");
            return -1;
        }
        g_vim.quit = 1;
        return 0;
    }
    if (p[0] == 'w' && p[1] == 'q' && p[2] == 0) {
        if (vim_save(0) != 0) {
            return -1;
        }
        g_vim.quit = 1;
        return 0;
    }
    if (p[0] == 'w' && (p[1] == 0 || p[1] == ' ')) {
        const char *path = 0;
        if (p[1] == ' ') {
            path = p + 2;
            while (*path == ' ') {
                path++;
            }
            if (*path == 0) {
                path = 0;
            }
        }
        return vim_save(path);
    }
    if (p[0] == 'e' && p[1] == ' ') {
        char abs[CLI_FS_PATH_MAX];
        const char *path = p + 2;
        while (*path == ' ') {
            path++;
        }
        if (*path == 0) {
            vim_set_msg("E: :e needs path");
            return -1;
        }
        if (g_vim.modified) {
            vim_set_msg("E: No write since last change");
            return -1;
        }
        if (cli_path_resolve(path, abs, sizeof(abs), 0) != 0) {
            return -1;
        }
        vim_free_all();
        if (vim_load(abs) != 0) {
            /* 恢复空缓冲以免崩溃 */
            g_vim.text = (char *)cgrtos_malloc(64);
            if (g_vim.text) {
                g_vim.cap = 64;
                g_vim.len = 0;
                vim_rebuild_lines();
            }
            return -1;
        }
        vim_set_msg("edited");
        return 0;
    }
    if (strncmp(p, "set ", 4) == 0) {
        p += 4;
        while (*p == ' ') {
            p++;
        }
        if (strcmp(p, "nu") == 0 || strcmp(p, "number") == 0) {
            g_vim.show_nu = 1;
            vim_set_msg("number");
            return 0;
        }
        if (strcmp(p, "nonu") == 0 || strcmp(p, "nonumber") == 0) {
            g_vim.show_nu = 0;
            vim_set_msg("nonumber");
            return 0;
        }
        vim_set_msg("E: unknown set option");
        return -1;
    }
    vim_set_msg("E: unknown command");
    return -1;
}

static void vim_enter_cmdline(int kind)
{
    g_vim.mode = VIM_CMDLINE;
    g_vim.cmdkind = kind;
    g_vim.cmdlen = 0;
    g_vim.cmdline[0] = 0;
    g_vim.msg[0] = 0;
}

static void vim_visual_bounds(size_t *a, size_t *b)
{
    *a = g_vim.vanchor;
    *b = g_vim.cur;
    if (*a > *b) {
        size_t t = *a;
        *a = *b;
        *b = t;
    }
    if (*b < g_vim.len) {
        (*b)++; /* inclusive end like vim charwise */
    }
}

static int vim_repeat_last(void)
{
    if (g_vim.last_op == 1 && g_vim.last_ins && g_vim.last_ins_len) {
        return vim_buf_insert(g_vim.cur, g_vim.last_ins, g_vim.last_ins_len, 1);
    }
    if (g_vim.last_op == 2) {
        if (g_vim.cur < g_vim.len && g_vim.text[g_vim.cur] != '\n') {
            return vim_buf_delete(g_vim.cur, 1, 1);
        }
        return 0;
    }
    if (g_vim.last_op == 3) {
        return vim_delete_lines(1);
    }
    if (g_vim.last_op == 4) {
        return vim_paste(1);
    }
    vim_set_msg("E: no previous command");
    return -1;
}

static void vim_remember_insert(const char *s, size_t n)
{
    cgrtos_free(g_vim.last_ins);
    g_vim.last_ins = 0;
    g_vim.last_ins_len = 0;
    g_vim.last_op = 1;
    if (n == 0) {
        return;
    }
    g_vim.last_ins = (char *)cgrtos_malloc(n + 1);
    if (!g_vim.last_ins) {
        return;
    }
    memcpy(g_vim.last_ins, s, n);
    g_vim.last_ins[n] = 0;
    g_vim.last_ins_len = n;
}

static int vim_pollc(void)
{
    for (;;) {
        int ch = cgrtos_uart_pollc();
        if (ch >= 0) {
            return ch;
        }
        cgrtos_task_yield();
    }
}

static char g_ins_acc[256];
static int  g_ins_acc_len;

static void vim_handle_insert(int ch)
{
    if (ch == 0x1B) {
        if (g_ins_acc_len > 0) {
            vim_remember_insert(g_ins_acc, (size_t)g_ins_acc_len);
            g_ins_acc_len = 0;
        }
        g_vim.mode = VIM_NORMAL;
        if (g_vim.cur > 0) {
            int row, col;
            vim_off_to_rc(g_vim.cur, &row, &col);
            if (col > 0) {
                g_vim.cur--;
            }
        }
        return;
    }
    if (ch == 0x03) {
        g_ins_acc_len = 0;
        g_vim.mode = VIM_NORMAL;
        vim_set_msg("Interrupted");
        return;
    }
    if (ch == 0x7F || ch == 0x08) {
        if (g_vim.cur > 0) {
            size_t p = g_vim.cur - 1;
            vim_buf_delete(p, 1, 1);
            if (g_ins_acc_len > 0) {
                g_ins_acc_len--;
            }
        }
        return;
    }
    if (ch == '\r') {
        ch = '\n';
    }
    if (ch >= 32 || ch == '\n' || ch == '\t') {
        char c = (char)ch;
        if (vim_buf_insert(g_vim.cur, &c, 1, 1) != 0) {
            return;
        }
        if (g_ins_acc_len + 1 < (int)sizeof(g_ins_acc)) {
            g_ins_acc[g_ins_acc_len++] = c;
        }
    }
}

static void vim_handle_cmdline(int ch)
{
    if (ch == 0x1B || ch == 0x03) {
        g_vim.mode = VIM_NORMAL;
        g_vim.cmdlen = 0;
        g_vim.cmdline[0] = 0;
        vim_set_msg(ch == 0x03 ? "Interrupted" : "");
        return;
    }
    if (ch == '\r' || ch == '\n') {
        g_vim.cmdline[g_vim.cmdlen] = 0;
        if (g_vim.cmdkind == ':') {
            vim_ex_run();
        } else if (g_vim.cmdkind == '/' || g_vim.cmdkind == '?') {
            strncpy(g_vim.pattern, g_vim.cmdline, sizeof(g_vim.pattern) - 1);
            g_vim.pattern[sizeof(g_vim.pattern) - 1] = 0;
            g_vim.search_fwd = (g_vim.cmdkind == '/');
            vim_search(g_vim.search_fwd, 1);
        }
        if (!g_vim.quit) {
            g_vim.mode = VIM_NORMAL;
        }
        g_vim.cmdlen = 0;
        g_vim.cmdline[0] = 0;
        return;
    }
    if (ch == 0x7F || ch == 0x08) {
        if (g_vim.cmdlen > 0) {
            g_vim.cmdlen--;
            g_vim.cmdline[g_vim.cmdlen] = 0;
        }
        return;
    }
    if (ch >= 32 && ch < 127 && g_vim.cmdlen + 1 < CGRTOS_VIM_CMDLINE_MAX) {
        g_vim.cmdline[g_vim.cmdlen++] = (char)ch;
        g_vim.cmdline[g_vim.cmdlen] = 0;
    }
}

static void vim_handle_visual(int ch)
{
    size_t a, b;
    int n = g_vim.count > 0 ? g_vim.count : 1;

    if (ch == 0x1B || ch == 0x03) {
        g_vim.mode = VIM_NORMAL;
        g_vim.count = 0;
        vim_set_msg(ch == 0x03 ? "Interrupted" : "");
        return;
    }
    if (ch == 'h') {
        vim_move_left(n);
        g_vim.count = 0;
        return;
    }
    if (ch == 'l') {
        vim_move_right(n);
        g_vim.count = 0;
        return;
    }
    if (ch == 'j') {
        vim_move_down(n);
        g_vim.count = 0;
        return;
    }
    if (ch == 'k') {
        vim_move_up(n);
        g_vim.count = 0;
        return;
    }
    if (ch == 'w') {
        vim_word_fwd(n);
        g_vim.count = 0;
        return;
    }
    if (ch == 'b') {
        vim_word_back(n);
        g_vim.count = 0;
        return;
    }
    if (ch == 'e') {
        vim_word_end(n);
        g_vim.count = 0;
        return;
    }
    if (ch == '0') {
        if (g_vim.count == 0) {
            vim_line_begin();
        } else {
            g_vim.count = g_vim.count * 10;
        }
        return;
    }
    if (ch == '$') {
        vim_line_end();
        g_vim.count = 0;
        return;
    }
    if (ch == 'd' || ch == 'x') {
        vim_visual_bounds(&a, &b);
        if (b > a) {
            vim_clip_set(g_vim.text + a, b - a, 0);
            vim_buf_delete(a, b - a, 1);
            g_vim.last_op = 2;
        }
        g_vim.mode = VIM_NORMAL;
        g_vim.count = 0;
        return;
    }
    if (ch == 'y') {
        vim_visual_bounds(&a, &b);
        if (b > a) {
            vim_clip_set(g_vim.text + a, b - a, 0);
            vim_set_msg("yanked");
        }
        g_vim.mode = VIM_NORMAL;
        g_vim.count = 0;
        return;
    }
    if (ch >= '1' && ch <= '9') {
        g_vim.count = g_vim.count * 10 + (ch - '0');
        return;
    }
}

static void vim_handle_normal(int ch)
{
    int n = g_vim.count > 0 ? g_vim.count : 1;

    if (ch == 0x03) {
        vim_set_msg("Type :q and press Enter to quit");
        return;
    }
    if (ch >= '1' && ch <= '9') {
        g_vim.count = g_vim.count * 10 + (ch - '0');
        return;
    }
    if (ch == '0' && g_vim.count > 0) {
        g_vim.count = g_vim.count * 10;
        return;
    }

    if (g_vim.pending == 'd') {
        g_vim.pending = 0;
        if (ch == 'd') {
            vim_delete_lines(n);
        } else {
            vim_set_msg("E: invalid operator");
        }
        g_vim.count = 0;
        return;
    }
    if (g_vim.pending == 'y') {
        g_vim.pending = 0;
        if (ch == 'y') {
            vim_yank_lines(n);
            vim_set_msg("yanked");
        } else {
            vim_set_msg("E: invalid operator");
        }
        g_vim.count = 0;
        return;
    }

    switch (ch) {
    case 'h':
        vim_move_left(n);
        break;
    case 'l':
        vim_move_right(n);
        break;
    case 'j':
        vim_move_down(n);
        break;
    case 'k':
        vim_move_up(n);
        break;
    case 'w':
        vim_word_fwd(n);
        break;
    case 'b':
        vim_word_back(n);
        break;
    case 'e':
        vim_word_end(n);
        break;
    case '0':
        vim_line_begin();
        break;
    case '^':
        vim_line_first_nonblank();
        break;
    case '$':
        vim_line_end();
        break;
    case 'G':
        if (g_vim.count > 0) {
            int row = g_vim.count - 1;
            if (row < 0) {
                row = 0;
            }
            if (row >= g_vim.nlines) {
                row = g_vim.nlines - 1;
            }
            g_vim.cur = vim_rc_to_off(row, 0);
        } else {
            g_vim.cur = vim_rc_to_off(g_vim.nlines - 1, 0);
        }
        break;
    case 'i':
        g_ins_acc_len = 0;
        g_vim.mode = VIM_INSERT;
        break;
    case 'I':
        vim_line_first_nonblank();
        g_ins_acc_len = 0;
        g_vim.mode = VIM_INSERT;
        break;
    case 'a':
        if (g_vim.cur < g_vim.len && g_vim.text[g_vim.cur] != '\n') {
            g_vim.cur++;
        }
        g_ins_acc_len = 0;
        g_vim.mode = VIM_INSERT;
        break;
    case 'A':
        vim_line_end();
        g_ins_acc_len = 0;
        g_vim.mode = VIM_INSERT;
        break;
    case 'o': {
        int row, col;
        size_t pos;
        char nl = '\n';
        vim_off_to_rc(g_vim.cur, &row, &col);
        (void)col;
        pos = g_vim.loff[row + 1];
        vim_buf_insert(pos, &nl, 1, 1);
        g_vim.cur = pos;
        g_ins_acc_len = 0;
        g_vim.mode = VIM_INSERT;
        break;
    }
    case 'O': {
        int row, col;
        size_t pos;
        char nl = '\n';
        vim_off_to_rc(g_vim.cur, &row, &col);
        (void)col;
        pos = g_vim.loff[row];
        vim_buf_insert(pos, &nl, 1, 1);
        g_vim.cur = pos;
        g_ins_acc_len = 0;
        g_vim.mode = VIM_INSERT;
        break;
    }
    case 'x':
        while (n-- > 0) {
            if (g_vim.cur < g_vim.len && g_vim.text[g_vim.cur] != '\n') {
                vim_buf_delete(g_vim.cur, 1, 1);
                g_vim.last_op = 2;
            }
        }
        break;
    case 'X':
        while (n-- > 0) {
            if (g_vim.cur > 0) {
                g_vim.cur--;
                if (g_vim.text[g_vim.cur] != '\n') {
                    vim_buf_delete(g_vim.cur, 1, 1);
                    g_vim.last_op = 2;
                } else {
                    g_vim.cur++;
                }
            }
        }
        break;
    case 'd':
        g_vim.pending = 'd';
        return; /* keep count */
    case 'D': {
        int row, col;
        size_t a, b;
        vim_off_to_rc(g_vim.cur, &row, &col);
        (void)col;
        a = g_vim.cur;
        b = g_vim.loff[row + 1];
        if (b > a && g_vim.text[b - 1] == '\n') {
            b--;
        }
        if (b > a) {
            vim_clip_set(g_vim.text + a, b - a, 0);
            vim_buf_delete(a, b - a, 1);
            g_vim.last_op = 2;
        }
        break;
    }
    case 'y':
        g_vim.pending = 'y';
        return;
    case 'Y':
        vim_yank_lines(n);
        vim_set_msg("yanked");
        break;
    case 'p':
        vim_paste(1);
        break;
    case 'P':
        vim_paste(0);
        break;
    case 'u':
        vim_do_undo();
        break;
    case '.':
        vim_repeat_last();
        break;
    case 'v':
        g_vim.mode = VIM_VISUAL;
        g_vim.vanchor = g_vim.cur;
        break;
    case '/':
        vim_enter_cmdline('/');
        break;
    case '?':
        vim_enter_cmdline('?');
        break;
    case 'n':
        vim_search(g_vim.search_fwd, 1);
        break;
    case 'N':
        vim_search(!g_vim.search_fwd, 1);
        break;
    case ':':
        vim_enter_cmdline(':');
        break;
    case 0x1B:
        g_vim.pending = 0;
        g_vim.count = 0;
        break;
    default:
        vim_set_msg("E: unknown key");
        break;
    }
    g_vim.count = 0;
}

/**
 * @brief 进入编辑器主循环
 * @param[in] abs 已解析绝对路径
 * @return 0 正常退出；-1 打开失败
 * @attention ❌ ISR；✅ 阻塞
 * @internal
 */
static int vim_edit_abs(const char *abs)
{
    int esc = 0;

    if (vim_load(abs) != 0) {
        cgrtos_printf("vim: %s\n", g_vim.msg[0] ? g_vim.msg : "open failed");
        vim_free_all();
        return -1;
    }
    g_vim.mode = VIM_NORMAL;
    vim_draw();

    while (!g_vim.quit) {
        int ch = vim_pollc();
        if (esc == 1) {
            if (ch == '[') {
                esc = 2;
                continue;
            }
            esc = 0;
            /* treat as Esc */
            ch = 0x1B;
        } else if (esc == 2) {
            esc = 0;
            if (ch == 'A') {
                ch = 'k';
            } else if (ch == 'B') {
                ch = 'j';
            } else if (ch == 'C') {
                ch = 'l';
            } else if (ch == 'D') {
                ch = 'h';
            } else {
                continue;
            }
        } else if (ch == 0x1B) {
            /* peek for CSI */
            int n2 = cgrtos_uart_pollc();
            if (n2 == '[') {
                esc = 2;
                continue;
            }
            if (n2 >= 0) {
                /* push back impossible — handle Esc then ignore orphan via mode */
                if (g_vim.mode == VIM_INSERT) {
                    vim_handle_insert(0x1B);
                    vim_draw();
                }
                ch = n2;
            } else {
                if (g_vim.mode == VIM_INSERT) {
                    vim_handle_insert(0x1B);
                } else if (g_vim.mode == VIM_VISUAL) {
                    vim_handle_visual(0x1B);
                } else if (g_vim.mode == VIM_CMDLINE) {
                    vim_handle_cmdline(0x1B);
                }
                vim_draw();
                continue;
            }
        }

        g_vim.msg[0] = 0;
        if (g_vim.mode == VIM_INSERT) {
            vim_handle_insert(ch);
        } else if (g_vim.mode == VIM_VISUAL) {
            vim_handle_visual(ch);
        } else if (g_vim.mode == VIM_CMDLINE) {
            vim_handle_cmdline(ch);
        } else {
            vim_handle_normal(ch);
        }
        if (!g_vim.quit) {
            vim_draw();
        }
    }

    cgrtos_uart_puts("\033[2J\033[H");
    vim_free_all();
    return 0;
}

int cli_vim_try_handle(char *line)
{
    const char *arg = 0;
    char abs[CLI_FS_PATH_MAX];
    const char *p;

    if (!line || !line[0]) {
        return 0;
    }
    if (strncmp(line, "vi", 2) == 0 && (line[2] == 0 || line[2] == ' ' || line[2] == '\t')) {
        arg = line + 2;
    } else if (strncmp(line, "vim", 3) == 0 && (line[3] == 0 || line[3] == ' ' || line[3] == '\t')) {
        arg = line + 3;
    } else if (strncmp(line, "edit", 4) == 0 && (line[4] == 0 || line[4] == ' ' || line[4] == '\t')) {
        arg = line + 4;
    } else {
        return 0;
    }
    while (*arg == ' ' || *arg == '\t') {
        arg++;
    }
    p = arg;
    if (*p == 0) {
        cgrtos_printf("usage: vi|vim|edit <file>\n");
        return 1;
    }
    if (cli_path_resolve(p, abs, sizeof(abs), 0) != 0) {
        return 1;
    }
    (void)vim_edit_abs(abs);
    return 1;
}

void cli_vim_help(void)
{
    cgrtos_printf("  vi|vim|edit <file>  POSIX vi (Esc/:w/:q); Tab path complete\n");
}

#endif /* CONFIG_CLI_VIM && CONFIG_CLI_FS */
