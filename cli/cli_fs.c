/**
 * @file cli_fs.c
 * @brief CLI 文件系统命令实现（基于 VFS）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * 路径支持绝对/相对；每 CLI 任务独立 CWD；规范化时折叠 `.`/`..` 并拒绝越界。
 * 大文件读写使用堆上临时缓冲；mkfs 与递归删除交互确认；长 IO 轮询 Ctrl-C。
 *
 * @section cli_fs_examples 使用示例
 * @code
 *   pwd
 *   mkdir /tmp
 *   cd /tmp
 *   touch a.txt
 *   cat > a.txt           # 单行写入后结束（本实现：echo 风格见 cat）
 *   echo 内容见： cat 写文件用 `cat > file` 简化为写入参数
 *   ls -l
 *   cp a.txt b.txt
 *   mv b.txt c.txt
 *   hexdump c.txt
 *   stat c.txt
 *   df
 *   mount
 *   sync
 *   mkfs ram /            # 需确认 y
 *   rm -r /tmp            # 需确认 y
 *   fhandle
 *   fbench 1024 50
 * @endcode
 */
#include "cli_fs.h"
#include "cli_path.h"
#include "cli_vim.h"

#if CONFIG_CLI_FS

#include "../kernel/vfs.h"
#include <string.h>

/**
 * @brief 大文件 IO 堆缓冲大小
 * @warning 无运行时副作用（编译期常量）
 */
#define CLI_FS_IO_CHUNK   512
/**
 * @brief CLI 跟踪的打开句柄数
 * @warning 无运行时副作用（编译期常量）
 */
#define CLI_FS_FH_MAX     8

/**
 * @brief CLI 打开句柄跟踪表项
 * @details 供 fhandle 列出本会话曾登记的 fd/path；非内核 fd 表。
 */
typedef struct {
    uint8_t used;                 /**< @brief 槽是否占用 */
    int     fd;                   /**< @brief VFS 文件描述符 */
    char    path[CLI_FS_PATH_MAX]; /**< @brief 打开时绝对路径快照 */
} cli_fs_fh_t;

/** @brief 本 CLI 任务跟踪的打开句柄表（无锁，单任务） */
static cli_fs_fh_t g_fh[CLI_FS_FH_MAX];

/**
 * @brief 本地字符串相等比较
 * @details 逐字节至双 NUL；两指针同为 NULL 视为相等。
 * @param[in] a 串 A
 * @param[in] b 串 B
 * @return 1 相等；0 不等
 * @retval 1 相等
 * @retval 0 不等
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static int fs_streq(const char *a, const char *b)
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
 * @brief 前缀匹配且后接空白或结束
 * @details 用于解析 `cmd args`；匹配后可选写出参数起始指针。
 * @param[in]  s      行
 * @param[in]  prefix 命令名
 * @param[out] rest   参数起始；可为 NULL
 * @return 1 匹配；0 否
 * @retval 1 匹配
 * @retval 0 否
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static int fs_startswith(const char *s, const char *prefix, const char **rest)
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
 * @brief 跳过前导空白
 * @details 前进至首个非空格/制表符，或指向 NUL。
 * @param[in] s 输入串
 * @return 指向首个非空白字符
 * @retval 非 NULL 始终有效（同 s 或其后）
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static const char *fs_skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/**
 * @brief 登记打开句柄路径（供 fhandle）
 * @details 在 g_fh 空闲槽写入 fd 与路径副本；表满则静默丢弃登记。
 * @param[in] fd   描述符
 * @param[in] path 绝对路径
 * @return 无
 * @retval 无
 * @note 表满则静默丢弃登记
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static void fs_fh_add(int fd, const char *path)
{
    if (fd < 0 || !path) {
        return;
    }
    for (int i = 0; i < CLI_FS_FH_MAX; i++) {
        if (!g_fh[i].used) {
            g_fh[i].used = 1;
            g_fh[i].fd = fd;
            strncpy(g_fh[i].path, path, CLI_FS_PATH_MAX);
            g_fh[i].path[CLI_FS_PATH_MAX - 1] = 0;
            return;
        }
    }
}

/**
 * @brief 移除句柄登记
 * @details 按 fd 匹配并清零对应 g_fh 槽。
 * @param[in] fd 描述符
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static void fs_fh_del(int fd)
{
    for (int i = 0; i < CLI_FS_FH_MAX; i++) {
        if (g_fh[i].used && g_fh[i].fd == fd) {
            memset(&g_fh[i], 0, sizeof(g_fh[i]));
        }
    }
}

/**
 * @brief 检测用户是否按 Ctrl-C 请求中止
 * @details 非阻塞 poll UART；若读到 0x03 则打印 ^C 并返回 1。
 * @return 1=中止；0=继续
 * @retval 1 中止
 * @retval 0 继续
 * @note 长 IO 循环中调用
 * @warning 会消耗一个输入字节
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static int fs_abort_poll(void)
{
    int ch = cgrtos_uart_pollc();
    if (ch == 0x03) {
        cgrtos_printf("^C\n");
        return 1;
    }
    return 0;
}

/**
 * @brief 交互确认危险操作
 * @details 打印提示后阻塞读取一行，仅 "y"/"Y"/"yes" 视为同意。
 * @param[in] prompt 提示语
 * @return 1=确认；0=取消
 * @retval 1 确认
 * @retval 0 取消
 * @note 使用 cgrtos_uart_getc
 * @warning 阻塞直到换行
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static int fs_confirm(const char *prompt)
{
    char line[16];
    int n = 0;
    cgrtos_printf("%s [y/N]: ", prompt ? prompt : "confirm");
    for (;;) {
        char c = cgrtos_uart_getc();
        if (c == '\r' || c == '\n') {
            cgrtos_uart_putc('\n');
            break;
        }
        if ((c == 0x7F || c == 0x08) && n > 0) {
            n--;
            cgrtos_uart_puts("\b \b");
            continue;
        }
        if (n + 1 < (int)sizeof(line) && c >= 32 && c < 127) {
            line[n++] = c;
            cgrtos_uart_putc(c);
        }
    }
    line[n] = 0;
    return fs_streq(line, "y") || fs_streq(line, "Y") || fs_streq(line, "yes");
}

/**
 * @brief 将用户路径解析为绝对路径
 * @details 包装 cli_path_resolve(quiet=0)；失败时由下层打印原因。
 * @param[in]  user   用户路径
 * @param[out] abs    输出绝对路径
 * @param[in]  abs_sz 容量
 * @return 0 成功；-1 失败
 * @retval 0  成功
 * @retval -1 失败
 * @note 不持 g_fs_mtx
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static int fs_resolve(const char *user, char *abs, size_t abs_sz)
{
    return cli_path_resolve(user, abs, abs_sz, 0);
}

/**
 * @brief 取下一空白分隔 token（原地切开）
 * @details 将首个空白写为 NUL，推进 *pp，返回 token 起始。
 * @param[in,out] pp 指向当前解析位置的指针
 * @return token 起始；无则 NULL
 * @retval 非 NULL token
 * @retval NULL    结束
 * @note 无
 * @warning 修改原字符串
 * @attention ✅ ISR；❌ block/switch
 * @internal
 */
static char *fs_next_tok(char **pp)
{
    char *s;
    char *t;
    if (!pp || !*pp) {
        return 0;
    }
    s = (char *)fs_skip_ws(*pp);
    if (*s == 0) {
        *pp = s;
        return 0;
    }
    t = s;
    while (*s && *s != ' ' && *s != '\t') {
        s++;
    }
    if (*s) {
        *s++ = 0;
    }
    *pp = s;
    return t;
}

/**
 * @brief 递归删除目录树
 * @details 深度优先；每步检查 Ctrl-C；目录非空则先删子项；拒绝删除 `/`。
 * @param[in] path 绝对路径
 * @return 0 成功；-1 失败；-2 用户中止
 * @retval 0  成功
 * @retval -1 失败
 * @retval -2 中止
 * @note 使用堆缓冲拼接子路径
 * @warning 危险
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static int fs_rm_tree(const char *path)
{
    cgrtos_stat_t st;
    cgrtos_dir_t *dir;
    cgrtos_dirent_t ent;
    char *child;

    if (fs_abort_poll()) {
        return -2;
    }
    if (vfs_stat(path, &st) != 0) {
        return -1;
    }
    if ((st.mode & CGRTOS_S_IFDIR) == 0) {
        return vfs_unlink(path);
    }
    if (fs_streq(path, "/")) {
        cgrtos_printf("fs: refuse to remove /\n");
        return -1;
    }
    dir = vfs_opendir(path);
    if (!dir) {
        return -1;
    }
    child = (char *)cgrtos_malloc(CLI_FS_PATH_MAX);
    if (!child) {
        vfs_closedir(dir);
        return -1;
    }
    while (vfs_readdir(dir, &ent) == 1) {
        int rc;
        if (fs_abort_poll()) {
            cgrtos_free(child);
            vfs_closedir(dir);
            return -2;
        }
        if (fs_streq(path, "/")) {
            cgrtos_snprintf(child, CLI_FS_PATH_MAX, "/%s", ent.name);
        } else {
            cgrtos_snprintf(child, CLI_FS_PATH_MAX, "%s/%s", path, ent.name);
        }
        rc = fs_rm_tree(child);
        if (rc != 0) {
            cgrtos_free(child);
            vfs_closedir(dir);
            return rc;
        }
    }
    cgrtos_free(child);
    vfs_closedir(dir);
    return vfs_rmdir(path);
}

/**
 * @brief 带跟踪的打开
 * @details 调用 vfs_open；成功则 fs_fh_add 登记路径。
 * @param[in] path  绝对路径
 * @param[in] flags 打开标志
 * @return fd 或 -1
 * @retval >=0 fd
 * @retval -1  失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static int fs_open_tracked(const char *path, int flags)
{
    int fd = vfs_open(path, flags);
    if (fd >= 0) {
        fs_fh_add(fd, path);
    }
    return fd;
}

/**
 * @brief 带跟踪的关闭
 * @details 先 fs_fh_del 再 vfs_close。
 * @param[in] fd 描述符
 * @return vfs_close 结果
 * @retval 0 成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static int fs_close_tracked(int fd)
{
    fs_fh_del(fd);
    return vfs_close(fd);
}

/**
 * @brief pwd：打印当前工作目录
 * @details 读取会话 CWD 并打印一行；无会话则报错。
 * @param[in] arg 忽略
 * @return 无
 * @retval 无
 * @note 示例：`pwd`
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_pwd(const char *arg)
{
    char cwd[CLI_FS_PATH_MAX];
    (void)arg;
    if (cli_path_getcwd(cwd, sizeof(cwd)) != 0) {
        cgrtos_printf("fs: no session\n");
        return;
    }
    cgrtos_printf("%s\n", cwd);
}

/**
 * @brief cd：切换工作目录
 * @details 解析路径，确认目标为目录后 cli_path_setcwd；空参数视为 `/`。
 * @param[in] arg 目标路径；空则 `/`
 * @return 无
 * @retval 无
 * @note 示例：`cd /tmp` / `cd ..`
 * @warning 目标须为目录
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_cd(const char *arg)
{
    char abs[CLI_FS_PATH_MAX];
    cgrtos_stat_t st;
    const char *p = fs_skip_ws(arg ? arg : "");

    if (*p == 0) {
        p = "/";
    }
    if (fs_resolve(p, abs, sizeof(abs)) != 0) {
        return;
    }
    if (vfs_stat(abs, &st) != 0 || (st.mode & CGRTOS_S_IFDIR) == 0) {
        cgrtos_printf("cd: not a directory: %s\n", abs);
        return;
    }
    if (cli_path_setcwd(abs) != 0) {
        cgrtos_printf("cd: setcwd failed\n");
    }
}

/**
 * @brief ls：列出目录（可选 -l）
 * @details 解析可选 `-l` 与路径；目录则 readdir；若为普通文件则打印该项。长列表含类型与大小。
 * @param[in] arg `[-l] [path]`
 * @return 无
 * @retval 无
 * @note 示例：`ls` / `ls -l /tmp`；Ctrl-C 可中止
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_ls(const char *arg)
{
    char *work;
    char *tok;
    char *p;
    int longfmt = 0;
    char abs[CLI_FS_PATH_MAX];
    cgrtos_dir_t *dir;
    cgrtos_dirent_t ent;
    const char *path_arg = "";

    work = (char *)cgrtos_malloc(CLI_FS_PATH_MAX);
    if (!work) {
        cgrtos_printf("ls: nomem\n");
        return;
    }
    strncpy(work, arg ? arg : "", CLI_FS_PATH_MAX);
    work[CLI_FS_PATH_MAX - 1] = 0;
    p = work;
    while ((tok = fs_next_tok(&p)) != 0) {
        if (fs_streq(tok, "-l")) {
            longfmt = 1;
        } else {
            path_arg = tok;
            break;
        }
    }
    if (fs_resolve(path_arg, abs, sizeof(abs)) != 0) {
        cgrtos_free(work);
        return;
    }
    dir = vfs_opendir(abs);
    if (!dir) {
        cgrtos_stat_t st;
        if (vfs_stat(abs, &st) == 0 && (st.mode & CGRTOS_S_IFREG)) {
                if (longfmt) {
                cgrtos_printf("- %u %s\n", (unsigned)st.size, abs);
            } else {
                cgrtos_printf("%s\n", abs);
            }
        } else {
            cgrtos_printf("ls: cannot open %s\n", abs);
        }
        cgrtos_free(work);
        return;
    }
    while (vfs_readdir(dir, &ent) == 1) {
        if (fs_abort_poll()) {
            break;
        }
        if (longfmt) {
            char full[CLI_FS_PATH_MAX];
            cgrtos_stat_t st;
            if (fs_streq(abs, "/")) {
                cgrtos_snprintf(full, sizeof(full), "/%s", ent.name);
            } else {
                cgrtos_snprintf(full, sizeof(full), "%s/%s", abs, ent.name);
            }
            if (vfs_stat(full, &st) == 0) {
                cgrtos_printf("%c %u %s\n",
                              (st.mode & CGRTOS_S_IFDIR) ? 'd' : '-',
                              (unsigned)st.size, ent.name);
            } else {
                cgrtos_printf("? %s\n", ent.name);
            }
        } else {
            cgrtos_printf("%s%s\n", ent.name,
                          (ent.mode & CGRTOS_S_IFDIR) ? "/" : "");
        }
    }
    vfs_closedir(dir);
    cgrtos_free(work);
}

/**
 * @brief cat：打印文件内容（堆缓冲分块）
 * @details 只读打开文件，按 CLI_FS_IO_CHUNK 分块读出并写 UART；循环中轮询 Ctrl-C。
 * @param[in] arg 文件路径
 * @return 无
 * @retval 无
 * @note 示例：`cat /tmp/a.txt`
 * @warning 二进制可能扰乱终端
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_cat(const char *arg)
{
    char abs[CLI_FS_PATH_MAX];
    int fd;
    uint8_t *buf;
    const char *p = fs_skip_ws(arg ? arg : "");

    if (*p == 0) {
        cgrtos_printf("usage: cat <file>\n");
        return;
    }
    if (fs_resolve(p, abs, sizeof(abs)) != 0) {
        return;
    }
    fd = fs_open_tracked(abs, CGRTOS_O_RDONLY);
    if (fd < 0) {
        cgrtos_printf("cat: open failed %s\n", abs);
        return;
    }
    buf = (uint8_t *)cgrtos_malloc(CLI_FS_IO_CHUNK);
    if (!buf) {
        fs_close_tracked(fd);
        cgrtos_printf("cat: nomem\n");
        return;
    }
    for (;;) {
        int n;
        if (fs_abort_poll()) {
            break;
        }
        n = vfs_read(fd, buf, CLI_FS_IO_CHUNK);
        if (n < 0) {
            cgrtos_printf("\ncat: read error\n");
            break;
        }
        if (n == 0) {
            break;
        }
        for (int i = 0; i < n; i++) {
            cgrtos_uart_putc((char)buf[i]);
        }
    }
    cgrtos_free(buf);
    fs_close_tracked(fd);
}

/**
 * @brief touch：创建空文件或确保存在
 * @details 以 CREAT|RDWR 打开后立即关闭；已存在则仅打开关闭。
 * @param[in] arg 路径
 * @return 无
 * @retval 无
 * @note 示例：`touch a.txt`
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_touch(const char *arg)
{
    char abs[CLI_FS_PATH_MAX];
    int fd;
    const char *p = fs_skip_ws(arg ? arg : "");
    if (*p == 0) {
        cgrtos_printf("usage: touch <file>\n");
        return;
    }
    if (fs_resolve(p, abs, sizeof(abs)) != 0) {
        return;
    }
    fd = fs_open_tracked(abs, CGRTOS_O_CREAT | CGRTOS_O_RDWR);
    if (fd < 0) {
        cgrtos_printf("touch: failed %s\n", abs);
        return;
    }
    fs_close_tracked(fd);
}

/**
 * @brief mkdir：创建目录
 * @details 解析绝对路径后调用 vfs_mkdir；不递归创建父目录。
 * @param[in] arg 路径
 * @return 无
 * @retval 无
 * @note 示例：`mkdir /tmp`
 * @warning 无父级递归创建
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_mkdir(const char *arg)
{
    char abs[CLI_FS_PATH_MAX];
    const char *p = fs_skip_ws(arg ? arg : "");
    if (*p == 0) {
        cgrtos_printf("usage: mkdir <dir>\n");
        return;
    }
    if (fs_resolve(p, abs, sizeof(abs)) != 0) {
        return;
    }
    if (vfs_mkdir(abs) != 0) {
        cgrtos_printf("mkdir: failed %s\n", abs);
    }
}

/**
 * @brief rm：删除文件；`rm -r` 递归删目录（需确认）
 * @details 普通文件 unlink；目录须 `-r`/`-R` 且交互确认后 fs_rm_tree。
 * @param[in] arg `[-r] <path>`
 * @return 无
 * @retval 无
 * @note 示例：`rm a.txt` / `rm -r /tmp`
 * @warning 递归删除危险
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_rm(const char *arg)
{
    char *work = (char *)cgrtos_malloc(CLI_FS_PATH_MAX);
    char *p;
    char *tok;
    int recursive = 0;
    char abs[CLI_FS_PATH_MAX];
    cgrtos_stat_t st;
    const char *path_arg = 0;

    if (!work) {
        cgrtos_printf("rm: nomem\n");
        return;
    }
    strncpy(work, arg ? arg : "", CLI_FS_PATH_MAX);
    work[CLI_FS_PATH_MAX - 1] = 0;
    p = work;
    while ((tok = fs_next_tok(&p)) != 0) {
        if (fs_streq(tok, "-r") || fs_streq(tok, "-R")) {
            recursive = 1;
        } else {
            path_arg = tok;
            break;
        }
    }
    if (!path_arg) {
        cgrtos_printf("usage: rm [-r] <path>\n");
        cgrtos_free(work);
        return;
    }
    if (fs_resolve(path_arg, abs, sizeof(abs)) != 0) {
        cgrtos_free(work);
        return;
    }
    if (vfs_stat(abs, &st) != 0) {
        cgrtos_printf("rm: no such file %s\n", abs);
        cgrtos_free(work);
        return;
    }
    if (st.mode & CGRTOS_S_IFDIR) {
        int rc;
        if (!recursive) {
            cgrtos_printf("rm: is directory (use -r)\n");
            cgrtos_free(work);
            return;
        }
        if (!fs_confirm("recursive delete")) {
            cgrtos_printf("aborted\n");
            cgrtos_free(work);
            return;
        }
        rc = fs_rm_tree(abs);
        if (rc == -2) {
            cgrtos_printf("rm: aborted\n");
        } else if (rc != 0) {
            cgrtos_printf("rm: failed %s\n", abs);
        }
    } else {
        if (vfs_unlink(abs) != 0) {
            cgrtos_printf("rm: failed %s\n", abs);
        }
    }
    cgrtos_free(work);
}

/**
 * @brief mv：重命名/移动
 * @details 解析 src/dst 绝对路径后 vfs_rename。
 * @param[in] arg `<src> <dst>`
 * @return 无
 * @retval 无
 * @note 示例：`mv a.txt b.txt`
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_mv(const char *arg)
{
    char *work = (char *)cgrtos_malloc(CLI_FS_PATH_MAX * 2);
    char *p;
    char *src;
    char *dst;
    char abs1[CLI_FS_PATH_MAX], abs2[CLI_FS_PATH_MAX];

    if (!work) {
        cgrtos_printf("mv: nomem\n");
        return;
    }
    strncpy(work, arg ? arg : "", CLI_FS_PATH_MAX * 2 - 1);
    work[CLI_FS_PATH_MAX * 2 - 1] = 0;
    p = work;
    src = fs_next_tok(&p);
    dst = fs_next_tok(&p);
    if (!src || !dst) {
        cgrtos_printf("usage: mv <src> <dst>\n");
        cgrtos_free(work);
        return;
    }
    if (fs_resolve(src, abs1, sizeof(abs1)) != 0 ||
        fs_resolve(dst, abs2, sizeof(abs2)) != 0) {
        cgrtos_free(work);
        return;
    }
    if (vfs_rename(abs1, abs2) != 0) {
        cgrtos_printf("mv: failed\n");
    }
    cgrtos_free(work);
}

/**
 * @brief cp：拷贝文件（堆缓冲分块，可 Ctrl-C）
 * @details 仅普通文件；分块 read/write；中止时留下部分目标文件。
 * @param[in] arg `<src> <dst>`
 * @return 无
 * @retval 无
 * @note 示例：`cp a.txt b.txt`
 * @warning 仅普通文件
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_cp(const char *arg)
{
    char *work = (char *)cgrtos_malloc(CLI_FS_PATH_MAX * 2);
    char *p, *src, *dst;
    char abs1[CLI_FS_PATH_MAX], abs2[CLI_FS_PATH_MAX];
    int ifd = -1, ofd = -1;
    uint8_t *buf = 0;
    cgrtos_stat_t st;

    if (!work) {
        cgrtos_printf("cp: nomem\n");
        return;
    }
    strncpy(work, arg ? arg : "", CLI_FS_PATH_MAX * 2 - 1);
    work[CLI_FS_PATH_MAX * 2 - 1] = 0;
    p = work;
    src = fs_next_tok(&p);
    dst = fs_next_tok(&p);
    if (!src || !dst) {
        cgrtos_printf("usage: cp <src> <dst>\n");
        goto out;
    }
    if (fs_resolve(src, abs1, sizeof(abs1)) != 0 ||
        fs_resolve(dst, abs2, sizeof(abs2)) != 0) {
        goto out;
    }
    if (vfs_stat(abs1, &st) != 0 || (st.mode & CGRTOS_S_IFDIR)) {
        cgrtos_printf("cp: source must be a file\n");
        goto out;
    }
    ifd = fs_open_tracked(abs1, CGRTOS_O_RDONLY);
    ofd = fs_open_tracked(abs2, CGRTOS_O_CREAT | CGRTOS_O_RDWR | CGRTOS_O_TRUNC);
    if (ifd < 0 || ofd < 0) {
        cgrtos_printf("cp: open failed\n");
        goto out;
    }
    buf = (uint8_t *)cgrtos_malloc(CLI_FS_IO_CHUNK);
    if (!buf) {
        cgrtos_printf("cp: nomem\n");
        goto out;
    }
    for (;;) {
        int n, w;
        if (fs_abort_poll()) {
            cgrtos_printf("cp: aborted\n");
            break;
        }
        n = vfs_read(ifd, buf, CLI_FS_IO_CHUNK);
        if (n < 0) {
            cgrtos_printf("cp: read error\n");
            break;
        }
        if (n == 0) {
            break;
        }
        w = vfs_write(ofd, buf, (size_t)n);
        if (w != n) {
            cgrtos_printf("cp: write error\n");
            break;
        }
    }
out:
    if (buf) {
        cgrtos_free(buf);
    }
    if (ifd >= 0) {
        fs_close_tracked(ifd);
    }
    if (ofd >= 0) {
        fs_close_tracked(ofd);
    }
    cgrtos_free(work);
}

/**
 * @brief stat：打印类型与大小
 * @details vfs_stat 后输出 path/type/size。
 * @param[in] arg 路径
 * @return 无
 * @retval 无
 * @note 示例：`stat /tmp/a.txt`
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_stat(const char *arg)
{
    char abs[CLI_FS_PATH_MAX];
    cgrtos_stat_t st;
    const char *p = fs_skip_ws(arg ? arg : "");
    if (*p == 0) {
        cgrtos_printf("usage: stat <path>\n");
        return;
    }
    if (fs_resolve(p, abs, sizeof(abs)) != 0) {
        return;
    }
    if (vfs_stat(abs, &st) != 0) {
        cgrtos_printf("stat: failed %s\n", abs);
        return;
    }
    cgrtos_printf("path=%s type=%s size=%u\n", abs,
                  (st.mode & CGRTOS_S_IFDIR) ? "dir" : "file",
                  (unsigned)st.size);
}

/**
 * @brief hexdump：十六进制转储文件
 * @details 每行 16 字节偏移、十六进制与可打印 ASCII；Ctrl-C 可中止。
 * @param[in] arg 路径
 * @return 无
 * @retval 无
 * @note 示例：`hexdump a.txt`
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_hexdump(const char *arg)
{
    char abs[CLI_FS_PATH_MAX];
    int fd;
    uint8_t *buf;
    uint32_t off = 0;
    const char *p = fs_skip_ws(arg ? arg : "");

    if (*p == 0) {
        cgrtos_printf("usage: hexdump <file>\n");
        return;
    }
    if (fs_resolve(p, abs, sizeof(abs)) != 0) {
        return;
    }
    fd = fs_open_tracked(abs, CGRTOS_O_RDONLY);
    if (fd < 0) {
        cgrtos_printf("hexdump: open failed\n");
        return;
    }
    buf = (uint8_t *)cgrtos_malloc(16);
    if (!buf) {
        fs_close_tracked(fd);
        return;
    }
    for (;;) {
        int n;
        if (fs_abort_poll()) {
            break;
        }
        n = vfs_read(fd, buf, 16);
        if (n <= 0) {
            break;
        }
        cgrtos_printf("%x:", (unsigned)off);
        for (int i = 0; i < n; i++) {
            cgrtos_printf(" %x", buf[i]);
        }
        cgrtos_printf("  |");
        for (int i = 0; i < n; i++) {
            char c = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
            cgrtos_uart_putc(c);
        }
        cgrtos_printf("|\n");
        off += (uint32_t)n;
    }
    cgrtos_free(buf);
    fs_close_tracked(fd);
}

/**
 * @brief mount：挂载或列示
 * @details 无参列出挂载表；有参则 `<fstype> <mp>` 调用 vfs_mount。
 * @param[in] arg 空=列表；否则 `<fstype> <mp>`
 * @return 无
 * @retval 无
 * @note 示例：`mount` / `mount ram /mnt`
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_mount(const char *arg)
{
    char *work;
    char *p, *fstype, *mp;
    const char *a = fs_skip_ws(arg ? arg : "");

    if (*a == 0) {
        vfs_mount_info_t list[VFS_MAX_MOUNTS];
        int n = vfs_list_mounts(list, VFS_MAX_MOUNTS);
        for (int i = 0; i < n; i++) {
            cgrtos_printf("%s on %s\n", list[i].fstype, list[i].mp);
        }
        return;
    }
    work = (char *)cgrtos_malloc(128);
    if (!work) {
        return;
    }
    strncpy(work, a, 127);
    work[127] = 0;
    p = work;
    fstype = fs_next_tok(&p);
    mp = fs_next_tok(&p);
    if (!fstype || !mp) {
        cgrtos_printf("usage: mount [<fstype> <mp>]\n");
        cgrtos_free(work);
        return;
    }
    if (vfs_mount(fstype, mp, 0) != 0) {
        cgrtos_printf("mount: failed (unsupported fstype or busy)\n");
    } else {
        cgrtos_printf("mounted %s on %s\n", fstype, mp);
    }
    cgrtos_free(work);
}

/**
 * @brief umount：卸载挂载点
 * @details 调用 vfs_umount；由 VFS 拒绝卸掉唯一根等非法操作。
 * @param[in] arg 挂载点
 * @return 无
 * @retval 无
 * @note 示例：`umount /mnt`
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_umount(const char *arg)
{
    const char *p = fs_skip_ws(arg ? arg : "");
    if (*p == 0) {
        cgrtos_printf("usage: umount <mp>\n");
        return;
    }
    if (vfs_umount(p) != 0) {
        cgrtos_printf("umount: failed\n");
    }
}

/**
 * @brief df：显示容量
 * @details 遍历挂载点 vfs_statfs；可选参数过滤单一挂载点。
 * @param[in] arg 可选挂载点
 * @return 无
 * @retval 无
 * @note 示例：`df` / `df /`
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_df(const char *arg)
{
    vfs_mount_info_t list[VFS_MAX_MOUNTS];
    int n;
    const char *only = fs_skip_ws(arg ? arg : "");

    n = vfs_list_mounts(list, VFS_MAX_MOUNTS);
    for (int i = 0; i < n; i++) {
        cgrtos_statfs_t st;
        if (*only && !fs_streq(only, list[i].mp)) {
            continue;
        }
        if (vfs_statfs(list[i].mp, &st) != 0) {
            cgrtos_printf("%s: statfs failed\n", list[i].mp);
            continue;
        }
        cgrtos_printf("%s (%s): blocks %u/%u  inodes %u/%u  maxfile %u\n",
                      list[i].mp, list[i].fstype,
                      (unsigned)st.blocks_used, (unsigned)st.blocks_total,
                      (unsigned)st.inodes_used, (unsigned)st.inodes_total,
                      (unsigned)st.max_file);
    }
}

/**
 * @brief sync：刷写挂载点
 * @details 空参数同步全部；否则同步指定挂载点。
 * @param[in] arg 可选挂载点；空=全部
 * @return 无
 * @retval 无
 * @note 示例：`sync`
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_sync(const char *arg)
{
    const char *p = fs_skip_ws(arg ? arg : "");
    if (vfs_sync(*p ? p : 0) != 0) {
        cgrtos_printf("sync: failed\n");
    } else {
        cgrtos_printf("sync: ok\n");
    }
}

/**
 * @brief mkfs：格式化（需确认）
 * @details 解析可选 fstype/mp（默认 ram /）；确认后 vfs_mkfs 并将 CWD 重置为 `/`。
 * @param[in] arg `[fstype] [mp]`，默认 `ram /`
 * @return 无
 * @retval 无
 * @note 示例：`mkfs ram /`
 * @warning 销毁全部数据
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_mkfs(const char *arg)
{
    char *work = (char *)cgrtos_malloc(128);
    char *p, *fstype, *mp;

    if (!work) {
        return;
    }
    strncpy(work, arg ? arg : "", 127);
    work[127] = 0;
    p = work;
    fstype = fs_next_tok(&p);
    mp = fs_next_tok(&p);
    if (!fstype) {
        fstype = "ram";
    }
    if (!mp) {
        mp = "/";
    }
    (void)fstype;
    if (!fs_confirm("mkfs will ERASE filesystem")) {
        cgrtos_printf("aborted\n");
        cgrtos_free(work);
        return;
    }
    if (vfs_mkfs(mp, 0) != 0) {
        cgrtos_printf("mkfs: failed\n");
    } else {
        cgrtos_printf("mkfs: ok on %s\n", mp);
        (void)cli_path_setcwd("/");
    }
    cgrtos_free(work);
}

/**
 * @brief fhandle：列出 CLI 跟踪的打开句柄
 * @details 打印 g_fh 中所有 used 槽的 fd 与 path；无则提示空。
 * @param[in] arg 保留
 * @return 无
 * @retval 无
 * @note 示例：`fhandle`
 * @warning 仅跟踪本模块 open/close
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_fhandle(const char *arg)
{
    int any = 0;
    (void)arg;
    for (int i = 0; i < CLI_FS_FH_MAX; i++) {
        if (g_fh[i].used) {
            cgrtos_printf("fd=%d path=%s\n", g_fh[i].fd, g_fh[i].path);
            any = 1;
        }
    }
    if (!any) {
        cgrtos_printf("(no tracked open handles)\n");
    }
}

/**
 * @brief fbench：简易文件读写基准
 * @details 在 `/tmp/fbench.bin` 上循环写读指定大小；统计 tick 差；可 Ctrl-C。
 * @param[in] arg `[size] [iters]`，默认 256 / 20
 * @return 无
 * @retval 无
 * @note 示例：`fbench 1024 50`
 * @warning 使用 `/tmp/fbench.bin`
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static void cmd_fbench(const char *arg)
{
    unsigned size = 256;
    unsigned iters = 20;
    char *work = (char *)cgrtos_malloc(64);
    char *p, *t1, *t2;
    uint8_t *buf;
    tick_t t0, t1tick;
    const char *path = "/tmp/fbench.bin";
    int fd;

    if (work) {
        unsigned parsed;
        strncpy(work, arg ? arg : "", 63);
        work[63] = 0;
        p = work;
        t1 = fs_next_tok(&p);
        t2 = fs_next_tok(&p);
        if (t1) {
            parsed = 0;
            for (const char *s = t1; *s >= '0' && *s <= '9'; s++) {
                parsed = parsed * 10U + (unsigned)(*s - '0');
            }
            if (parsed != 0) {
                size = parsed;
            }
        }
        if (t2) {
            parsed = 0;
            for (const char *s = t2; *s >= '0' && *s <= '9'; s++) {
                parsed = parsed * 10U + (unsigned)(*s - '0');
            }
            if (parsed != 0) {
                iters = parsed;
            }
        }
        cgrtos_free(work);
    }
    if (size > CGRTOS_FS_MAX_FILE_BYTES) {
        size = CGRTOS_FS_MAX_FILE_BYTES;
    }
    vfs_mkdir("/tmp");
    buf = (uint8_t *)cgrtos_malloc(size ? size : 1);
    if (!buf) {
        cgrtos_printf("fbench: nomem\n");
        return;
    }
    memset(buf, 0xA5, size);
    t0 = cgrtos_get_ticks();
    for (unsigned i = 0; i < iters; i++) {
        if (fs_abort_poll()) {
            cgrtos_printf("fbench: aborted\n");
            cgrtos_free(buf);
            return;
        }
        fd = vfs_open(path, CGRTOS_O_CREAT | CGRTOS_O_RDWR | CGRTOS_O_TRUNC);
        if (fd < 0) {
            cgrtos_printf("fbench: open failed\n");
            break;
        }
        if (vfs_write(fd, buf, size) < 0) {
            cgrtos_printf("fbench: write failed\n");
            vfs_close(fd);
            break;
        }
        vfs_lseek(fd, 0, 0);
        if (vfs_read(fd, buf, size) < 0) {
            cgrtos_printf("fbench: read failed\n");
            vfs_close(fd);
            break;
        }
        vfs_close(fd);
    }
    t1tick = cgrtos_get_ticks();
    vfs_unlink(path);
    cgrtos_printf("fbench: size=%u iters=%u ticks=%lu\n",
                  size, iters, (unsigned long)(t1tick - t0));
    cgrtos_free(buf);
}

/**
 * @brief 初始化本 CLI 任务的文件系统会话（CWD=/）
 * @details 调用 cli_path_session_init 并 vfs_init；重复调用重置 CWD 为根。
 * @return 无
 * @retval 无
 * @note 在 cli_task 启动时调用一次
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
void cli_fs_session_init(void)
{
    cli_path_session_init();
    vfs_init();
}

/**
 * @brief 向帮助文本追加 FS 命令说明与示例
 * @details 由 cli_help 在 CONFIG_CLI_FS=1 时调用，经 UART 打印命令表。
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
void cli_fs_help(void)
{
    cgrtos_printf("\nFilesystem (VFS) commands: (CONFIG_CLI_FS)\n");
    cgrtos_printf("  pwd | cd <path> | ls [-l] [path]\n");
    cgrtos_printf("  cat <file> | touch <file> | mkdir <dir>\n");
    cgrtos_printf("  rm [-r] <path> | mv <src> <dst> | cp <src> <dst>\n");
    cgrtos_printf("  stat <path> | hexdump <file>\n");
    cgrtos_printf("  mount [<fstype> <mp>] | umount <mp> | df [mp]\n");
    cgrtos_printf("  sync [mp] | mkfs [ram] [/] | fhandle | fbench [sz] [n]\n");
    cgrtos_printf("  vi|vim|edit <file>   (CONFIG_CLI_VIM; see docs/MODULE_CLI_VIM.md)\n");
    cgrtos_printf("  Tab completes paths; Ctrl-C aborts long IO; mkfs/rm -r ask confirm\n");
    cgrtos_printf("Examples:\n");
    cgrtos_printf("  mkdir /tmp && cd /tmp && touch a.txt\n");
    cgrtos_printf("  vi a.txt ; ls -l / ; cat /tmp/a.txt\n");
    cgrtos_printf("  df ; mount ; sync ; fbench 512 30\n\n");
}

/**
 * @brief 尝试分发文件系统命令
 * @details 识别 pwd/cd/ls/cat/… 及 vi；处理成功返回 1；非 FS 命令返回 0。
 * @param[in] line 已 trim 的命令行
 * @return 1=已处理；0=非本模块命令
 * @retval 1 已处理
 * @retval 0 未识别为 FS 命令
 * @note 所有文件操作走 vfs_*
 * @warning mkfs / rm -r 会交互确认
 * @attention ❌ ISR；✅ block/switch
 */
int cli_fs_try_handle(char *line)
{
    const char *arg = 0;

    if (!line || !line[0]) {
        return 0;
    }
    if (fs_streq(line, "pwd")) {
        cmd_pwd(0);
        return 1;
    }
    if (fs_startswith(line, "cd", &arg)) {
        cmd_cd(arg);
        return 1;
    }
    if (fs_streq(line, "ls") || fs_startswith(line, "ls", &arg)) {
        cmd_ls(arg ? arg : "");
        return 1;
    }
    if (fs_startswith(line, "cat", &arg)) {
        cmd_cat(arg);
        return 1;
    }
    if (fs_startswith(line, "touch", &arg)) {
        cmd_touch(arg);
        return 1;
    }
    if (fs_startswith(line, "mkdir", &arg)) {
        cmd_mkdir(arg);
        return 1;
    }
    if (fs_startswith(line, "rm", &arg)) {
        cmd_rm(arg);
        return 1;
    }
    if (fs_startswith(line, "mv", &arg)) {
        cmd_mv(arg);
        return 1;
    }
    if (fs_startswith(line, "cp", &arg)) {
        cmd_cp(arg);
        return 1;
    }
    if (fs_startswith(line, "stat", &arg)) {
        cmd_stat(arg);
        return 1;
    }
    if (fs_startswith(line, "hexdump", &arg)) {
        cmd_hexdump(arg);
        return 1;
    }
    if (fs_streq(line, "mount") || fs_startswith(line, "mount", &arg)) {
        cmd_mount(arg ? arg : "");
        return 1;
    }
    if (fs_startswith(line, "umount", &arg)) {
        cmd_umount(arg);
        return 1;
    }
    if (fs_streq(line, "df") || fs_startswith(line, "df", &arg)) {
        cmd_df(arg ? arg : "");
        return 1;
    }
    if (fs_streq(line, "sync") || fs_startswith(line, "sync", &arg)) {
        cmd_sync(arg ? arg : "");
        return 1;
    }
    if (fs_streq(line, "mkfs") || fs_startswith(line, "mkfs", &arg)) {
        cmd_mkfs(arg ? arg : "");
        return 1;
    }
    if (fs_streq(line, "fhandle") || fs_startswith(line, "fhandle", &arg)) {
        cmd_fhandle(arg ? arg : "");
        return 1;
    }
    if (fs_streq(line, "fbench") || fs_startswith(line, "fbench", &arg)) {
        cmd_fbench(arg ? arg : "");
        return 1;
    }
    if (cli_vim_try_handle(line)) {
        return 1;
    }
    return 0;
}

#endif /* CONFIG_CLI_FS */
