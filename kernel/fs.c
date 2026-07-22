/**
 * @file fs.c
 * @brief 纯 RAM 文件系统模块
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 本模块实现嵌入式场景下的轻量级内存文件系统：
 *
 * - **inode 池** `g_inos[]`：固定大小数组，inode 0 为根目录 `/`；每个 inode
 *   持有堆分配的 `data` 缓冲（文件内容或目录项打包数据）。
 * - **目录项格式**：`[ino:u16 LE][namelen:u8][name...]` 连续打包于目录 inode 的 data 中。
 * - **fd 表** `g_fds[]`：打开文件描述符，记录 inode 号、读写位置与 flags。
 * - **目录迭代器** `g_dirs[]`：最多 8 个并发 opendir 句柄，记录目录 inode 与遍历偏移。
 *
 * 仅支持绝对路径；`path_resolve` 用栈模拟目录遍历，支持 `.` 与 `..`。
 * 全局互斥锁 `g_fs_mtx` 串行化所有 API；文件/目录数据通过 TLSF 堆动态扩容。
 *
 * @see cgrtos_fs
 */
#include "cgrtos.h"
#include <string.h>

/** @name lseek whence（内部，与常见 POSIX 一致） */
/** @{ */
#define FS_SEEK_SET 0
#define FS_SEEK_CUR 1
#define FS_SEEK_END 2
/** @} */

/** @brief inode：普通文件或目录 */
typedef struct fs_inode {
    uint8_t  used;       /**< 池槽占用 */
    uint8_t  is_dir;     /**< 1=目录 */
    uint16_t nlink;      /**< 链接计数（简化实现） */
    uint32_t size;       /**< 有效字节 / 目录打包长度 */
    uint8_t *data;       /**< 堆缓冲 */
    uint32_t capacity;   /**< data 分配容量 */
} fs_inode_t;

/** @brief 打开文件描述符 */
typedef struct {
    uint8_t    used;
    uint16_t   ino;
    uint32_t   pos;
    int        flags;
} fs_fd_t;

/** @brief 目录迭代状态（不透明 `cgrtos_dir_t`） */
struct cgrtos_dir {
    uint8_t  used;
    uint16_t ino;
    uint32_t pos; /**< 在目录 data 中的字节偏移 */
};

static fs_inode_t g_inos[CGRTOS_FS_MAX_INODES];
static fs_fd_t    g_fds[CGRTOS_FS_MAX_FD];
static struct cgrtos_dir g_dirs[8];
static cgrtos_mutex_t *g_fs_mtx;
static uint8_t g_fs_ready;

/**
 * @brief 获取文件系统全局互斥锁
 * @details 若互斥锁已创建，以 portMAX_DELAY 阻塞加锁，串行化后续文件系统操作。
 * @return 无
 * @retval 无
 * @note 所有公开 API 经 fs_lock/fs_unlock 包裹
 * @warning 持锁期间禁止再次 fs_lock（非递归锁）
 * @attention ❌ ISR；✅ block
 * @internal
 */
static void fs_lock(void)
{
    /* 1. 若互斥锁已创建，以 portMAX_DELAY 阻塞加锁 */
    if (g_fs_mtx) {
        cgrtos_mutex_lock(g_fs_mtx, portMAX_DELAY);
    }
}

/**
 * @brief 释放文件系统全局互斥锁
 * @details 若互斥锁已创建，调用 unlock 释放，允许其他任务进入文件系统 API。
 * @return 无
 * @retval 无
 * @note 必须与 fs_lock 成对调用
 * @warning 未持锁时 unlock 行为未定义
 * @attention ❌ ISR；❌ block
 * @internal
 */
static void fs_unlock(void)
{
    /* 1. 若互斥锁已创建，调用 unlock */
    if (g_fs_mtx) {
        cgrtos_mutex_unlock(g_fs_mtx);
    }
}

/**
 * @brief 分配空闲 inode（跳过 inode 0 根目录）
 * @details 从 inode 1 起线性扫描 g_inos，找 used==0 的槽；清零后置 used=1、is_dir、nlink=1 并返回 inode 号；池满返回 -1。
 * @param[in] is_dir 1 创建目录 inode；0 创建普通文件
 * @return 新 inode 号；池满返回 -1
 * @retval >=1 新分配的 inode 号
 * @retval -1   inode 池已满
 * @note 调用方须已持 fs 锁
 * @warning 不分配 data 缓冲，由后续写入或目录操作触发
 * @attention ❌ ISR；❌ block
 * @internal
 */
static int16_t ino_alloc(int is_dir)
{
    /* 1. 从 inode 1 起线性扫描 g_inos，找 used==0 的槽 */
    for (uint16_t i = 1; i < CGRTOS_FS_MAX_INODES; i++) {
        if (!g_inos[i].used) {
            /* 2. 清零结构，置 used=1、is_dir、nlink=1 */
            memset(&g_inos[i], 0, sizeof(g_inos[i]));
            g_inos[i].used = 1;
            g_inos[i].is_dir = is_dir ? 1 : 0;
            g_inos[i].nlink = 1;
            /* 3. 返回 inode 号 */
            return (int16_t)i;
        }
    }
    /* 3. 无空闲槽返回 -1 */
    return -1;
}

/**
 * @brief 释放 inode 及其堆数据
 * @details 拒绝释放 inode 0（根）、越界或未使用的槽；若 data 非空则 cgrtos_free，最后 memset 清零 inode 结构。
 * @param[in] ino inode 号
 * @return 无
 * @retval 无
 * @note 不更新父目录 dentry，由 dir_remove_child 先行处理
 * @warning 释放仍在 fd 表或目录项中引用的 inode 将导致悬空引用
 * @attention ❌ ISR；❌ block
 * @internal
 */
static void ino_free(uint16_t ino)
{
    /* 1. 拒绝释放 inode 0（根）、越界或未使用的槽 */
    if (ino == 0 || ino >= CGRTOS_FS_MAX_INODES || !g_inos[ino].used) {
        return;
    }
    /* 2. 若 data 非空则 cgrtos_free */
    if (g_inos[ino].data) {
        cgrtos_free(g_inos[ino].data);
    }
    /* 3. memset 清零 inode 结构 */
    memset(&g_inos[ino], 0, sizeof(g_inos[ino]));
}

/**
 * @brief 解析绝对路径，可选在末级创建文件或目录
 * @details 校验 path 以 '/' 开头；用 stack[16] 维护当前目录 inode；逐组件解析，支持 `.`/`..`；末级缺失时按 create_file/create_dir 分配 inode 并追加 dentry。
 * @param[in]  path        绝对路径（以 '/' 开头）
 * @param[in]  create_file 末级缺失时是否创建普通文件
 * @param[in]  create_dir  末级缺失时是否创建目录
 * @param[out] out_ino     输出目标 inode 号（可为 NULL）
 * @param[out] out_parent  输出父目录 inode 号（可为 NULL）
 * @param[out] out_leaf    输出末级组件名（可为 NULL）
 * @return 0 成功；-1 错误；-2 叶节点缺失且未请求创建
 * @retval 0  路径解析成功
 * @retval -1 参数非法、中间路径不存在或栈溢出
 * @retval -2 末级不存在且未启用创建
 * @note 路径深度上限 16 层
 * @warning 并发调用须由 fs_lock 保护
 * @attention ❌ ISR；❌ block
 * @internal
 */
static int path_resolve(const char *path, int create_file, int create_dir,
                        int16_t *out_ino, int16_t *out_parent, char *out_leaf)
{
    /* 1. 路径必须以 '/' 开头 */
    if (!path || path[0] != '/') {
        return -1;
    }

    int16_t stack[16];
    int sp = 0;
    stack[sp++] = 0;

    const char *p = path + 1;
    char comp[CGRTOS_FS_MAX_NAME];

    /* 2. 根路径 "/" */
    if (*p == 0) {
        if (out_ino) {
            *out_ino = 0;
        }
        if (out_parent) {
            *out_parent = 0;
        }
        if (out_leaf) {
            out_leaf[0] = 0;
        }
        return 0;
    }

    while (*p) {
        /* 3. 提取路径组件 */
        size_t n = 0;
        while (p[n] && p[n] != '/') {
            if (n + 1 >= sizeof(comp)) {
                return -1;
            }
            comp[n] = p[n];
            n++;
        }
        comp[n] = 0;
        p += n;
        while (*p == '/') {
            p++;
        }

        int last = (*p == 0);
        if (comp[0] == 0 || (comp[0] == '.' && comp[1] == 0)) {
            continue;
        }
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            if (sp > 1) {
                sp--;
            }
            if (last) {
                if (out_ino) {
                    *out_ino = stack[sp - 1];
                }
                return 0;
            }
            continue;
        }

        int16_t cur = stack[sp - 1];
        if (!g_inos[cur].used || !g_inos[cur].is_dir) {
            return -1;
        }

        /* 4. 在当前目录中查找组件 */
        uint8_t *d = g_inos[cur].data;
        uint32_t sz = g_inos[cur].size;
        uint32_t off = 0;
        int16_t found = -1;
        while (off + 3 <= sz) {
            uint16_t child = (uint16_t)d[off] | ((uint16_t)d[off + 1] << 8);
            uint8_t nl = d[off + 2];
            off += 3;
            if (off + nl > sz) {
                break;
            }
            if (nl == n) {
                size_t k;
                int match = 1;
                for (k = 0; k < n; k++) {
                    if (d[off + k] != (uint8_t)comp[k]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    found = (int16_t)child;
                }
            }
            off += nl;
            if (found >= 0) {
                break;
            }
        }

        if (found < 0) {
            /* 5/6. 未找到：中间路径错误或末级创建 */
            if (!last) {
                return -1;
            }
            if (out_parent) {
                *out_parent = cur;
            }
            if (out_leaf) {
                size_t i;
                for (i = 0; i < CGRTOS_FS_MAX_NAME - 1 && comp[i]; i++) {
                    out_leaf[i] = comp[i];
                }
                out_leaf[i] = 0;
            }
            if (create_file || create_dir) {
                int16_t ni = ino_alloc(create_dir ? 1 : 0);
                if (ni < 0) {
                    return -1;
                }
                /* 扩展父目录并追加 dentry */
                uint8_t nl = (uint8_t)n;
                uint32_t need = 3 + nl;
                if (g_inos[cur].size + need > g_inos[cur].capacity) {
                    uint32_t ncap = g_inos[cur].capacity ? g_inos[cur].capacity * 2 : 64;
                    while (ncap < g_inos[cur].size + need) {
                        ncap *= 2;
                    }
                    uint8_t *nd = (uint8_t *)cgrtos_malloc(ncap);
                    if (!nd) {
                        ino_free((uint16_t)ni);
                        return -1;
                    }
                    if (g_inos[cur].data) {
                        memcpy(nd, g_inos[cur].data, g_inos[cur].size);
                        cgrtos_free(g_inos[cur].data);
                    }
                    g_inos[cur].data = nd;
                    g_inos[cur].capacity = ncap;
                }
                uint8_t *dst = g_inos[cur].data + g_inos[cur].size;
                dst[0] = (uint8_t)(ni & 0xFF);
                dst[1] = (uint8_t)((ni >> 8) & 0xFF);
                dst[2] = nl;
                memcpy(dst + 3, comp, nl);
                g_inos[cur].size += need;
                if (out_ino) {
                    *out_ino = ni;
                }
                return 0;
            }
            if (out_ino) {
                *out_ino = -1;
            }
            return -2; /* missing leaf */
        }

        /* 7. 找到：入栈或到达末级 */
        if (sp >= 16) {
            return -1;
        }
        stack[sp++] = found;
        if (last) {
            if (out_ino) {
                *out_ino = found;
            }
            if (out_parent) {
                *out_parent = cur;
            }
            if (out_leaf) {
                size_t i;
                for (i = 0; i < CGRTOS_FS_MAX_NAME - 1 && comp[i]; i++) {
                    out_leaf[i] = comp[i];
                }
                out_leaf[i] = 0;
            }
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 从父目录中移除指定名称的子目录项
 * @details 校验 parent 为目录且 data 非空；线性扫描 `[ino:u16][nlen:u8][name...]` 格式目录项；名称匹配则用 memmove 移除并递减 size。
 * @param[in] parent 父目录 inode 号
 * @param[in] name   子项名称
 * @return 0 成功；-1 未找到或参数无效
 * @retval 0  目录项已移除
 * @retval -1 父目录无效或未找到匹配项
 * @note 调用方须已持 fs 锁
 * @warning 不释放子 inode，由调用方 ino_free
 * @attention ❌ ISR；❌ block
 * @internal
 */
static int dir_remove_child(int16_t parent, const char *name)
{
    fs_inode_t *dir = &g_inos[parent];
    if (!dir->is_dir || !dir->data) {
        return -1;
    }
    size_t nlen = 0;
    while (name[nlen]) {
        nlen++;
    }
    uint32_t off = 0;
    while (off + 3 <= dir->size) {
        uint16_t child = (uint16_t)dir->data[off] | ((uint16_t)dir->data[off + 1] << 8);
        uint8_t nl = dir->data[off + 2];
        uint32_t ent = 3 + nl;
        if (off + ent > dir->size) {
            break;
        }
        if (nl == nlen) {
            int match = 1;
            for (size_t k = 0; k < nlen; k++) {
                if (dir->data[off + 3 + k] != (uint8_t)name[k]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                /* 3. 移除匹配的目录项 */
                uint32_t rest = dir->size - (off + ent);
                if (rest) {
                    memmove(dir->data + off, dir->data + off + ent, rest);
                }
                dir->size -= ent;
                (void)child;
                return 0;
            }
        }
        off += ent;
    }
    return -1;
}

/**
 * @brief 向父目录追加子目录项
 * @details 扩展父目录缓冲后写入 `[ino:u16 LE][namelen][name]`。
 * @param[in] parent    父目录 inode
 * @param[in] child_ino 子 inode 号
 * @param[in] name      组件名
 * @return 0 成功；-1 失败
 * @retval 0  已追加
 * @retval -1 参数非法或 malloc 失败
 * @note 调用方须已持 fs 锁；不检查重名
 * @warning 重名由调用方保证不发生
 * @attention ❌ ISR；❌ block
 * @internal
 */
static int dir_add_child(int16_t parent, uint16_t child_ino, const char *name)
{
    fs_inode_t *dir = &g_inos[parent];
    size_t nlen = 0;
    uint8_t nl;
    uint32_t need;
    uint8_t *dst;

    if (!dir->used || !dir->is_dir || !name || !name[0]) {
        return -1;
    }
    while (name[nlen]) {
        nlen++;
    }
    if (nlen == 0 || nlen >= CGRTOS_FS_MAX_NAME) {
        return -1;
    }
    nl = (uint8_t)nlen;
    need = 3 + nl;
    if (dir->size + need > dir->capacity) {
        uint32_t ncap = dir->capacity ? dir->capacity * 2 : 64;
        uint8_t *nd;
        while (ncap < dir->size + need) {
            ncap *= 2;
        }
        nd = (uint8_t *)cgrtos_malloc(ncap);
        if (!nd) {
            return -1;
        }
        if (dir->data) {
            memcpy(nd, dir->data, dir->size);
            cgrtos_free(dir->data);
        }
        dir->data = nd;
        dir->capacity = ncap;
    }
    dst = dir->data + dir->size;
    dst[0] = (uint8_t)(child_ino & 0xFF);
    dst[1] = (uint8_t)((child_ino >> 8) & 0xFF);
    dst[2] = nl;
    memcpy(dst + 3, name, nl);
    dir->size += need;
    return 0;
}

/**
 * @brief 初始化 RAM 文件系统
 * @details 若已初始化则直接返回；清零 inode 池、fd 表、目录迭代器池；初始化 inode 0 为根目录；创建全局互斥锁并置 g_fs_ready=1。
 * @return 无
 * @retval 无
 * @note 幂等，可重复调用
 * @warning 重复 init 不会重置已有文件数据
 * @attention ❌ ISR；❌ block
 */
void cgrtos_fs_init(void)
{
    if (g_fs_ready) {
        return;
    }
    memset(g_inos, 0, sizeof(g_inos));
    memset(g_fds, 0, sizeof(g_fds));
    memset(g_dirs, 0, sizeof(g_dirs));
    g_inos[0].used = 1;
    g_inos[0].is_dir = 1;
    g_inos[0].nlink = 1;
    g_fs_mtx = cgrtos_mutex_create();
    g_fs_ready = 1;
}

/**
 * @brief 打开文件并分配文件描述符
 * @details 若未初始化则先 init；加锁后 path_resolve，O_CREAT 时在末级缺失时创建普通文件；O_TRUNC 清零 size；在 fd 池分配槽并记录 ino/flags/pos。
 * @param[in] path  绝对路径
 * @param[in] flags 打开标志（O_CREAT/O_TRUNC/O_APPEND/O_RDONLY 等）
 * @return 成功返回 fd（>=0）；失败返回 -1
 * @retval >=0 有效文件描述符
 * @retval -1  路径错误、目标是目录或 fd 池满
 * @note O_APPEND 打开时 pos 初始化为文件末尾
 * @warning 目录不可通过 open 打开
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_open(const char *path, int flags)
{
    /* 1. 若未初始化则先 cgrtos_fs_init */
    if (!g_fs_ready) {
        cgrtos_fs_init();
    }
    fs_lock();
    int16_t ino = -1, parent = -1;
    char leaf[CGRTOS_FS_MAX_NAME];
    int create = (flags & CGRTOS_O_CREAT) ? 1 : 0;
    /* 2. path_resolve；若 O_CREAT 则在末级缺失时创建普通文件 */
    int rc = path_resolve(path, create, 0, &ino, &parent, leaf);
    /* 3. 解析失败或目标是目录则 unlock 返回 -1 */
    if (rc < 0 || ino < 0) {
        fs_unlock();
        return -1;
    }
    if (g_inos[ino].is_dir) {
        fs_unlock();
        return -1;
    }
    /* 4. O_TRUNC 时将文件 size 清零 */
    if (flags & CGRTOS_O_TRUNC) {
        g_inos[ino].size = 0;
    }
    /* 5. 在 fd 池中找空闲槽，记录 ino/flags/pos */
    int fd = -1;
    for (int i = 0; i < CGRTOS_FS_MAX_FD; i++) {
        if (!g_fds[i].used) {
            g_fds[i].used = 1;
            g_fds[i].ino = (uint16_t)ino;
            g_fds[i].flags = flags;
            g_fds[i].pos = (flags & CGRTOS_O_APPEND) ? g_inos[ino].size : 0;
            fd = i;
            break;
        }
    }
    /* 6. 解锁返回 fd；fd 池满返回 -1 */
    fs_unlock();
    return fd;
}

/**
 * @brief 关闭文件描述符
 * @details 校验 fd 范围与 used 标志；加锁后 memset 清零 fd 槽，解锁返回 0。
 * @param[in] fd 文件描述符
 * @return 0 成功；-1 失败
 * @retval 0  fd 已关闭
 * @retval -1 fd 非法或未打开
 * @note 不释放 inode 或文件数据
 * @warning 关闭后 fd 号可能被复用
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_close(int fd)
{
    if (fd < 0 || fd >= CGRTOS_FS_MAX_FD) {
        return -1;
    }
    fs_lock();
    if (!g_fds[fd].used) {
        fs_unlock();
        return -1;
    }
    memset(&g_fds[fd], 0, sizeof(g_fds[fd]));
    fs_unlock();
    return 0;
}

/**
 * @brief 从打开的文件读取数据
 * @details 校验 fd/buf 与 fd.used；加锁后若 pos >= size 返回 0（EOF）；否则读取 min(size-pos, len) 字节并推进 pos。
 * @param[in]  fd  文件描述符
 * @param[out] buf 接收缓冲
 * @param[in]  len 期望读取字节数
 * @return 实际读取字节数（EOF 为 0）；错误返回 -1
 * @retval >=0 实际读取字节数
 * @retval -1  fd 或 buf 非法
 * @note 部分读取合法，调用方须循环直至满足需求
 * @warning buf 须有足够空间容纳返回值
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_read(int fd, void *buf, size_t len)
{
    /* 1. 校验 fd/buf 与 fd.used */
    if (fd < 0 || fd >= CGRTOS_FS_MAX_FD || !buf) {
        return -1;
    }
    fs_lock();
    if (!g_fds[fd].used) {
        fs_unlock();
        return -1;
    }
    fs_inode_t *ino = &g_inos[g_fds[fd].ino];
    /* 2. 若 pos >= ino.size 则返回 0（EOF） */
    if (g_fds[fd].pos >= ino->size) {
        fs_unlock();
        return 0;
    }
    /* 3. n = min(size - pos, len)；memcpy 到 buf，pos += n */
    uint32_t n = ino->size - g_fds[fd].pos;
    if (n > len) {
        n = (uint32_t)len;
    }
    if (n && ino->data) {
        memcpy(buf, ino->data + g_fds[fd].pos, n);
        g_fds[fd].pos += n;
    }
    /* 4. 解锁返回 n */
    fs_unlock();
    return (int)n;
}

/**
 * @brief 向打开的文件写入数据
 * @details 校验 fd/buf；O_RDONLY 拒绝写入；O_APPEND 将 pos 设为 size；超 CGRTOS_FS_MAX_FILE_BYTES 失败；必要时倍增扩容后 memcpy 写入并更新 size。
 * @param[in] fd  文件描述符
 * @param[in] buf 源数据
 * @param[in] len 写入字节数
 * @return 成功返回 len；错误返回 -1
 * @retval (int)len 全部写入成功
 * @retval -1       参数非法、只读打开、超限或 malloc 失败
 * @note 写入可扩展文件 size
 * @warning 并发写同一 fd 由互斥锁串行化，不同 fd 同一 inode 无额外保护
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_write(int fd, const void *buf, size_t len)
{
    /* 1. 校验 fd/buf 与 fd.used；O_RDONLY 拒绝写入 */
    if (fd < 0 || fd >= CGRTOS_FS_MAX_FD || !buf) {
        return -1;
    }
    fs_lock();
    if (!g_fds[fd].used) {
        fs_unlock();
        return -1;
    }
    if ((g_fds[fd].flags & 0x3) == CGRTOS_O_RDONLY) {
        fs_unlock();
        return -1;
    }
    fs_inode_t *ino = &g_inos[g_fds[fd].ino];
    /* 2. O_APPEND 时将 pos 设为 ino.size */
    if (g_fds[fd].flags & CGRTOS_O_APPEND) {
        g_fds[fd].pos = ino->size;
    }
    /* 3. 计算 end = pos + len，超过上限则失败 */
    uint32_t end = g_fds[fd].pos + (uint32_t)len;
    if (end > CGRTOS_FS_MAX_FILE_BYTES) {
        fs_unlock();
        return -1;
    }
    /* 4. 若 end > capacity，倍增扩容，malloc 并拷贝旧数据 */
    if (end > ino->capacity) {
        uint32_t ncap = ino->capacity ? ino->capacity * 2 : 64;
        while (ncap < end) {
            ncap *= 2;
        }
        if (ncap > CGRTOS_FS_MAX_FILE_BYTES) {
            ncap = CGRTOS_FS_MAX_FILE_BYTES;
        }
        uint8_t *nd = (uint8_t *)cgrtos_malloc(ncap);
        if (!nd) {
            fs_unlock();
            return -1;
        }
        if (ino->data) {
            memcpy(nd, ino->data, ino->size);
            cgrtos_free(ino->data);
        }
        ino->data = nd;
        ino->capacity = ncap;
    }
    /* 5. memcpy 写入，更新 pos；若 pos > size 则扩展 size */
    memcpy(ino->data + g_fds[fd].pos, buf, len);
    g_fds[fd].pos += (uint32_t)len;
    if (g_fds[fd].pos > ino->size) {
        ino->size = g_fds[fd].pos;
    }
    /* 6. 解锁返回 len */
    fs_unlock();
    return (int)len;
}

/**
 * @brief 移动文件读写位置
 * @details 校验 fd 与 used；按 whence（SET/CUR/END）计算新 pos；非法 whence 返回 -1；pos < 0 钳制为 0 后写回 fd.pos。
 * @param[in] fd     文件描述符
 * @param[in] off    偏移量
 * @param[in] whence FS_SEEK_SET/CUR/END
 * @return 新位置；错误返回 -1
 * @retval >=0 新的文件偏移
 * @retval -1  fd 非法或 whence 无效
 * @note SEEK_END 基于当前 ino->size
 * @warning seek 超出 size 合法，后续 write 会扩展文件
 * @attention ❌ ISR；✅ block
 */
long cgrtos_fs_lseek(int fd, long off, int whence)
{
    if (fd < 0 || fd >= CGRTOS_FS_MAX_FD) {
        return -1;
    }
    fs_lock();
    if (!g_fds[fd].used) {
        fs_unlock();
        return -1;
    }
    fs_inode_t *ino = &g_inos[g_fds[fd].ino];
    long pos = (long)g_fds[fd].pos;
    if (whence == FS_SEEK_SET) {
        pos = off;
    } else if (whence == FS_SEEK_CUR) {
        pos += off;
    } else if (whence == FS_SEEK_END) {
        pos = (long)ino->size + off;
    } else {
        fs_unlock();
        return -1;
    }
    if (pos < 0) {
        pos = 0;
    }
    g_fds[fd].pos = (uint32_t)pos;
    fs_unlock();
    return pos;
}

/**
 * @brief 创建目录
 * @details 若未初始化则 init；加锁后先 path_resolve 检测是否已存在，再 path_resolve(create_dir=1) 在末级创建目录 inode。
 * @param[in] path 绝对路径
 * @return 0 成功；-1 失败（已存在或路径错误）
 * @retval 0  目录已创建
 * @retval -1 已存在、路径非法或 inode 池满
 * @note 不递归创建中间目录（中间路径须已存在）
 * @warning 与 mkdir -p 语义不同
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_mkdir(const char *path)
{
    if (!g_fs_ready) {
        cgrtos_fs_init();
    }
    fs_lock();
    int16_t ino = -1;
    int rc = path_resolve(path, 0, 0, &ino, 0, 0);
    if (rc == 0 && ino >= 0) {
        fs_unlock();
        return -1; /* already exists */
    }
    ino = -1;
    rc = path_resolve(path, 0, 1, &ino, 0, 0);
    fs_unlock();
    return (rc == 0 && ino > 0) ? 0 : -1;
}

/**
 * @brief 删除普通文件（unlink）
 * @details 加锁后 path_resolve 定位 ino/parent/leaf；目标须为普通文件；dir_remove_child 移除 dentry 后 ino_free 释放资源。
 * @param[in] path 绝对路径
 * @return 0 成功；-1 失败
 * @retval 0  文件已删除
 * @retval -1 路径错误、目标是目录或 dentry 移除失败
 * @note 简化实现不维护 nlink，直接释放 inode
 * @warning 仍有 fd 打开时删除将导致 fd 悬空
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_unlink(const char *path)
{
    fs_lock();
    int16_t ino = -1, parent = -1;
    char leaf[CGRTOS_FS_MAX_NAME];
    /* 1. path_resolve 定位 ino/parent/leaf */
    int rc = path_resolve(path, 0, 0, &ino, &parent, leaf);
    /* 2. 目标须为普通文件（非目录），否则失败 */
    if (rc != 0 || ino <= 0 || g_inos[ino].is_dir) {
        fs_unlock();
        return -1;
    }
    /* 3. dir_remove_child 从父目录移除 dentry */
    if (dir_remove_child(parent, leaf) != 0) {
        fs_unlock();
        return -1;
    }
    /* 4. ino_free 释放 inode 与 data，解锁返回 0 */
    ino_free((uint16_t)ino);
    fs_unlock();
    return 0;
}

/**
 * @brief 删除空目录
 * @details 加锁后 path_resolve 定位目标；须为目录且 size==0；dir_remove_child 移除 dentry 后 ino_free。
 * @param[in] path 绝对路径
 * @return 0 成功；-1 失败
 * @retval 0  空目录已删除
 * @retval -1 非目录、非空或路径错误
 * @note 非空目录（含子项）拒绝删除
 * @warning 不检查是否有 opendir 句柄仍打开
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_rmdir(const char *path)
{
    fs_lock();
    int16_t ino = -1, parent = -1;
    char leaf[CGRTOS_FS_MAX_NAME];
    int rc = path_resolve(path, 0, 0, &ino, &parent, leaf);
    if (rc != 0 || ino <= 0 || !g_inos[ino].is_dir) {
        fs_unlock();
        return -1;
    }
    if (g_inos[ino].size != 0) {
        fs_unlock();
        return -1; /* not empty */
    }
    if (dir_remove_child(parent, leaf) != 0) {
        fs_unlock();
        return -1;
    }
    ino_free((uint16_t)ino);
    fs_unlock();
    return 0;
}

/**
 * @brief 获取路径对应 inode 的状态信息
 * @details 校验 st 非空；加锁后 path_resolve 定位 ino；填充 st->mode（目录/普通文件）与 st->size。
 * @param[in]  path 绝对路径
 * @param[out] st   输出 stat 结构
 * @return 0 成功；-1 失败
 * @retval 0  stat 已填充
 * @retval -1 st 为空或路径不存在
 * @note 仅填充 mode 与 size 字段
 * @warning 无
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_stat(const char *path, cgrtos_stat_t *st)
{
    if (!st) {
        return -1;
    }
    fs_lock();
    int16_t ino = -1;
    int rc = path_resolve(path, 0, 0, &ino, 0, 0);
    if (rc != 0 || ino < 0) {
        fs_unlock();
        return -1;
    }
    st->mode = g_inos[ino].is_dir ? CGRTOS_S_IFDIR : CGRTOS_S_IFREG;
    st->size = g_inos[ino].size;
    fs_unlock();
    return 0;
}

/**
 * @brief 打开目录以供 readdir 迭代
 * @details 加锁后 path_resolve 定位 ino，须为目录；在 g_dirs[8] 中分配 used==0 的槽，记录 ino 与 pos=0。
 * @param[in] path 绝对路径
 * @return 目录句柄；失败返回 NULL
 * @retval 非 NULL 有效目录句柄
 * @retval NULL    非目录、路径错误或句柄池满（最多 8 个）
 * @note 须 paired 调用 closedir
 * @warning 并发 opendir 同一目录共享 inode 数据
 * @attention ❌ ISR；✅ block
 */
cgrtos_dir_t *cgrtos_fs_opendir(const char *path)
{
    fs_lock();
    int16_t ino = -1;
    if (path_resolve(path, 0, 0, &ino, 0, 0) != 0 || ino < 0 ||
        !g_inos[ino].is_dir) {
        fs_unlock();
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (!g_dirs[i].used) {
            g_dirs[i].used = 1;
            g_dirs[i].ino = (uint16_t)ino;
            g_dirs[i].pos = 0;
            fs_unlock();
            return &g_dirs[i];
        }
    }
    fs_unlock();
    return 0;
}

/**
 * @brief 读取目录下一项
 * @details 校验 dir/out 与 dir.used；加锁后若 pos 达末尾返回 0（EOF）；解析 dentry 填充 out->name/mode 并推进 pos。
 * @param[in]  dir 目录句柄
 * @param[out] out 输出 dirent（name/mode）
 * @return 1 成功读一项；0 EOF；-1 错误
 * @retval 1  成功读取一项
 * @retval 0  目录遍历结束（EOF）
 * @retval -1 参数非法或目录 data 损坏
 * @note 不返回 "." 与 ".."（目录项仅含实际子项）
 * @warning 迭代期间目录被修改可能导致不一致
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_readdir(cgrtos_dir_t *dir, cgrtos_dirent_t *out)
{
    if (!dir || !out || !dir->used) {
        return -1;
    }
    fs_lock();
    fs_inode_t *d = &g_inos[dir->ino];
    if (dir->pos + 3 > d->size) {
        fs_unlock();
        return 0; /* EOF */
    }
    uint8_t *p = d->data + dir->pos;
    uint16_t child = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    uint8_t nl = p[2];
    if (dir->pos + 3 + nl > d->size || nl >= CGRTOS_FS_MAX_NAME) {
        fs_unlock();
        return -1;
    }
    memcpy(out->name, p + 3, nl);
    out->name[nl] = 0;
    out->mode = g_inos[child].is_dir ? CGRTOS_S_IFDIR : CGRTOS_S_IFREG;
    dir->pos += 3 + nl;
    fs_unlock();
    return 1;
}

/**
 * @brief 关闭目录句柄
 * @details 校验 dir 非空；加锁后 memset 清零 dir 结构，解锁返回 0。
 * @param[in] dir 目录句柄
 * @return 0 成功；-1 失败
 * @retval 0  句柄已关闭
 * @retval -1 dir 为 NULL
 * @note 关闭后句柄槽可被后续 opendir 复用
 * @warning 无
 * @attention ❌ ISR；✅ block
 */
int cgrtos_fs_closedir(cgrtos_dir_t *dir)
{
    if (!dir) {
        return -1;
    }
    fs_lock();
    memset(dir, 0, sizeof(*dir));
    fs_unlock();
    return 0;
}

/**
 * @brief 强制格式化 RAM FS（清空全部 inode/fd，重建根目录）
 * @details 关闭所有打开 fd 与目录句柄，释放堆数据后重新初始化根。
 * @return 0 成功；-1 失败
 * @retval 0  卷已清空
 * @retval -1 内部错误
 * @note 幂等；会丢弃全部用户数据
 * @warning 危险操作；CLI 须交互确认
 * @attention ❌ ISR；✅ 可能阻塞（互斥）
 */
int cgrtos_fs_format(void)
{
    if (!g_fs_ready) {
        cgrtos_fs_init();
    }
    fs_lock();
    for (int i = 0; i < CGRTOS_FS_MAX_FD; i++) {
        memset(&g_fds[i], 0, sizeof(g_fds[i]));
    }
    for (int i = 0; i < 8; i++) {
        memset(&g_dirs[i], 0, sizeof(g_dirs[i]));
    }
    for (uint16_t i = 0; i < CGRTOS_FS_MAX_INODES; i++) {
        if (g_inos[i].data) {
            cgrtos_free(g_inos[i].data);
        }
        memset(&g_inos[i], 0, sizeof(g_inos[i]));
    }
    g_inos[0].used = 1;
    g_inos[0].is_dir = 1;
    g_inos[0].nlink = 1;
    fs_unlock();
    return 0;
}

/**
 * @brief RAM FS 刷盘（内存后端为空操作屏障）
 * @details 持锁确保无进行中的写，作为 VFS sync 后端。
 * @return 0
 * @retval 0 成功
 * @note 无持久化介质时无实际 I/O
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
int cgrtos_fs_sync(void)
{
    if (!g_fs_ready) {
        cgrtos_fs_init();
    }
    fs_lock();
    fs_unlock();
    return 0;
}

/**
 * @brief 查询 RAM FS 用量
 * @details 统计 used inode 与 data size，按 512B 换算逻辑块。
 * @param[out] st 输出结构；不可为 NULL
 * @return 0 成功；-1 参数非法
 * @retval 0  已填充
 * @retval -1 st 为空
 * @note 供 df/fbench 使用
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
int cgrtos_fs_statfs(cgrtos_statfs_t *st)
{
    uint32_t used_bytes = 0;
    uint32_t inodes_used = 0;
    uint32_t total_bytes;

    if (!st) {
        return -1;
    }
    if (!g_fs_ready) {
        cgrtos_fs_init();
    }
    fs_lock();
    for (uint16_t i = 0; i < CGRTOS_FS_MAX_INODES; i++) {
        if (g_inos[i].used) {
            inodes_used++;
            used_bytes += g_inos[i].size;
        }
    }
    total_bytes = (uint32_t)CGRTOS_FS_MAX_INODES * (uint32_t)CGRTOS_FS_MAX_FILE_BYTES;
    st->blocks_total = (total_bytes + 511U) / 512U;
    st->blocks_used = (used_bytes + 511U) / 512U;
    st->inodes_total = CGRTOS_FS_MAX_INODES;
    st->inodes_used = inodes_used;
    st->name_max = CGRTOS_FS_MAX_NAME - 1;
    st->max_file = CGRTOS_FS_MAX_FILE_BYTES;
    fs_unlock();
    return 0;
}

/**
 * @brief 重命名/移动文件或目录（同卷内）
 * @details 从旧父目录摘除 dentry，挂到新父目录；禁止覆盖已存在目标。
 * @param[in] oldpath 源绝对路径
 * @param[in] newpath 目标绝对路径
 * @return 0 成功；-1 失败
 * @retval 0  已移动
 * @retval -1 路径非法、目标已存在或参数错误
 * @note 打开中的 fd 仍指向同一 inode
 * @warning 简化实现：目标父目录须已存在
 * @attention ❌ ISR；✅ 可能阻塞
 */
int cgrtos_fs_rename(const char *oldpath, const char *newpath)
{
    int16_t oino = -1, oparent = -1;
    int16_t nexist = -1, nparent = -1;
    char oleaf[CGRTOS_FS_MAX_NAME];
    char nleaf[CGRTOS_FS_MAX_NAME];
    char parent_path[256];
    size_t len = 0;
    int last_slash = -1;
    int rc;

    if (!oldpath || !newpath || oldpath[0] != '/' || newpath[0] != '/') {
        return -1;
    }
    fs_lock();
    rc = path_resolve(oldpath, 0, 0, &oino, &oparent, oleaf);
    if (rc != 0 || oino <= 0 || oleaf[0] == 0) {
        fs_unlock();
        return -1;
    }
    rc = path_resolve(newpath, 0, 0, &nexist, 0, 0);
    if (rc == 0 && nexist >= 0) {
        fs_unlock();
        return -1; /* target exists */
    }

    while (newpath[len] && len + 1 < sizeof(parent_path)) {
        if (newpath[len] == '/') {
            last_slash = (int)len;
        }
        parent_path[len] = newpath[len];
        len++;
    }
    parent_path[len] = 0;
    if (last_slash < 0) {
        fs_unlock();
        return -1;
    }
    if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = 0;
    } else {
        parent_path[last_slash] = 0;
    }
    {
        const char *leaf = newpath + last_slash + 1;
        size_t i = 0;
        if (!leaf[0]) {
            fs_unlock();
            return -1;
        }
        while (leaf[i] && leaf[i] != '/' && i + 1 < sizeof(nleaf)) {
            nleaf[i] = leaf[i];
            i++;
        }
        nleaf[i] = 0;
        if (leaf[i] == '/') {
            fs_unlock();
            return -1; /* trailing components not allowed */
        }
    }
    if (path_resolve(parent_path, 0, 0, &nparent, 0, 0) != 0 ||
        nparent < 0 || !g_inos[nparent].is_dir) {
        fs_unlock();
        return -1;
    }
    if (dir_remove_child(oparent, oleaf) != 0) {
        fs_unlock();
        return -1;
    }
    if (dir_add_child(nparent, (uint16_t)oino, nleaf) != 0) {
        (void)dir_add_child(oparent, (uint16_t)oino, oleaf);
        fs_unlock();
        return -1;
    }
    fs_unlock();
    return 0;
}
