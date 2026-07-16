/**
 * @file fs.c
 * @brief 纯 RAM 文件系统模块
 *
 * ## 模块设计
 *
 * 本模块实现嵌入式场景下的轻量级内存文件系统：
 *
 * - **inode 池** `g_inos[]`：固定大小数组，inode 0 为根目录 `/`；每个 inode
 *   持有堆分配的 `data` 缓冲（文件内容或目录项打包数据）。
 * - **目录项格式**：`[ino:u16 LE][namelen:u8][name...]` 连续打包于目录 inode 的 data 中。
 * - **fd 表** `g_fds[]`：打开文件描述符，记录 inode 号、读写位置与 flags。
 * - **目录迭代器** `g_dirs[]`：最多 8 个并发 opendir 句柄，记录目录 inode 与遍历偏移。
 *
 * ## 路径与并发
 *
 * - 仅支持绝对路径；`path_resolve` 用栈模拟目录遍历，支持 `.` 与 `..`。
 * - 全局互斥锁 `g_fs_mtx` 串行化所有 API；`fs_lock`/`fs_unlock` 封装加解锁。
 * - 文件/目录数据通过 TLSF 堆动态扩容（倍增策略）。
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
 *
 * @details
 * 1. 若互斥锁已创建，以 portMAX_DELAY 阻塞加锁。
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
 *
 * @details
 * 1. 若互斥锁已创建，调用 unlock。
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
 *
 * @param is_dir 1 创建目录 inode；0 创建普通文件
 * @return 新 inode 号；池满返回 -1
 *
 * @details
 * 1. 从 inode 1 起线性扫描 g_inos，找 used==0 的槽。
 * 2. 清零结构，置 used=1、is_dir、nlink=1。
 * 3. 返回 inode 号；无空闲槽返回 -1。
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
 *
 * @param ino inode 号
 *
 * @details
 * 1. 拒绝释放 inode 0（根）、越界或未使用的槽。
 * 2. 若 data 非空则 cgrtos_free。
 * 3. memset 清零 inode 结构。
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
 *
 * @param path        绝对路径（以 '/' 开头）
 * @param create_file 末级缺失时是否创建普通文件
 * @param create_dir  末级缺失时是否创建目录
 * @param out_ino     输出目标 inode 号（可为 NULL）
 * @param out_parent  输出父目录 inode 号（可为 NULL）
 * @param out_leaf    输出末级组件名（可为 NULL）
 * @return 0 成功；-1 错误；-2 叶节点缺失且未请求创建
 *
 * @details
 * 1. 校验 path 以 '/' 开头；用 stack[16] 维护当前目录 inode，初始 push 0（根）。
 * 2. 路径仅为 "/" 时直接返回 inode 0。
 * 3. 逐组件解析：提取 comp，跳过空与 "."； ".." 则 pop 栈（保留根）。
 * 4. 在当前目录 data 中线性搜索目录项，匹配 ino 与 comp 名。
 * 5. 未找到且非末级组件：返回 -1（中间路径不存在）。
 * 6. 未找到且为末级：若 create_file/create_dir，分配新 inode、扩展父目录 data、
 *    追加 dentry `[ino][nlen][name]`，返回 0；否则 out_ino=-1，返回 -2。
 * 7. 找到则 push 到栈；末级组件时填充 out_ino/out_parent/out_leaf 并返回 0。
 * 8. 栈溢出（深度>=16）或当前节点非目录则返回 -1。
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
 *
 * @param parent 父目录 inode 号
 * @param name   子项名称
 * @return 0 成功；-1 未找到或参数无效
 *
 * @details
 * 1. 校验 parent 为目录且 data 非空。
 * 2. 线性扫描目录项，格式 `[ino:u16][nlen:u8][name...]`。
 * 3. 名称匹配则用 memmove 移除该条目，size -= ent。
 * 4. 未找到返回 -1。
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
 * @brief 初始化 RAM 文件系统
 *
 * @details
 * 1. 若已初始化（g_fs_ready）则直接返回。
 * 2. 清零 inode 池、fd 表、目录迭代器池。
 * 3. 初始化 inode 0 为根目录（used/is_dir/nlink）。
 * 4. 创建全局互斥锁，置 g_fs_ready=1。
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
 *
 * @param path  绝对路径
 * @param flags 打开标志（O_CREAT/O_TRUNC/O_APPEND/O_RDONLY 等）
 * @return 成功返回 fd（>=0）；失败返回 -1
 *
 * @details
 * 1. 若未初始化则先 cgrtos_fs_init。
 * 2. 加锁，path_resolve；若 O_CREAT 则在末级缺失时创建普通文件。
 * 3. 解析失败或目标是目录则 unlock 返回 -1。
 * 4. O_TRUNC 时将文件 size 清零。
 * 5. 在 fd 池中找空闲槽，记录 ino/flags/pos（O_APPEND 则 pos=size）。
 * 6. 解锁返回 fd；fd 池满返回 -1。
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
 *
 * @param fd 文件描述符
 * @return 0 成功；-1 失败
 *
 * @details
 * 1. 校验 fd 范围与 used 标志。
 * 2. 加锁，memset 清零 fd 槽，解锁返回 0。
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
 *
 * @param fd  文件描述符
 * @param buf 接收缓冲
 * @param len 期望读取字节数
 * @return 实际读取字节数（EOF 为 0）；错误返回 -1
 *
 * @details
 * 1. 校验 fd/buf 与 fd.used。
 * 2. 加锁，若 pos >= ino.size 则返回 0（EOF）。
 * 3. n = min(size - pos, len)；memcpy 到 buf，pos += n。
 * 4. 解锁返回 n。
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
 *
 * @param fd  文件描述符
 * @param buf 源数据
 * @param len 写入字节数
 * @return 成功返回 len；错误返回 -1
 *
 * @details
 * 1. 校验 fd/buf 与 fd.used；O_RDONLY 拒绝写入。
 * 2. O_APPEND 时将 pos 设为 ino.size。
 * 3. 计算 end = pos + len，超过 CGRTOS_FS_MAX_FILE_BYTES 则失败。
 * 4. 若 end > capacity，倍增扩容（上限 MAX_FILE_BYTES），malloc 并拷贝旧数据。
 * 5. memcpy 写入，更新 pos；若 pos > size 则扩展 size。
 * 6. 解锁返回 len。
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
 *
 * @param fd     文件描述符
 * @param off    偏移量
 * @param whence FS_SEEK_SET/CUR/END
 * @return 新位置；错误返回 -1
 *
 * @details
 * 1. 校验 fd 与 used。
 * 2. 按 whence 计算 pos：SET=off，CUR=pos+off，END=size+off。
 * 3. 非法 whence 返回 -1；pos < 0 则钳制为 0。
 * 4. 更新 fd.pos，返回新 pos。
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
 *
 * @param path 绝对路径
 * @return 0 成功；-1 失败（已存在或路径错误）
 *
 * @details
 * 1. 若未初始化则 init；加锁。
 * 2. 先 path_resolve 不创建：若已存在（rc==0 && ino>=0）则失败。
 * 3. 再 path_resolve(create_dir=1) 在末级创建目录 inode。
 * 4. 成功条件：rc==0 且 ino>0；解锁返回。
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
 *
 * @param path 绝对路径
 * @return 0 成功；-1 失败
 *
 * @details
 * 1. 加锁，path_resolve 定位 ino/parent/leaf。
 * 2. 目标须为普通文件（非目录），否则失败。
 * 3. dir_remove_child 从父目录移除 dentry。
 * 4. ino_free 释放 inode 与 data，解锁返回 0。
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
 *
 * @param path 绝对路径
 * @return 0 成功；-1 失败
 *
 * @details
 * 1. 加锁，path_resolve 定位 ino/parent/leaf。
 * 2. 目标须为目录；size!=0（非空）则失败。
 * 3. dir_remove_child 从父目录移除 dentry。
 * 4. ino_free 释放目录 inode，解锁返回 0。
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
 *
 * @param path 绝对路径
 * @param st   输出 stat 结构
 * @return 0 成功；-1 失败
 *
 * @details
 * 1. 校验 st 非空；加锁。
 * 2. path_resolve 定位 ino；失败则返回 -1。
 * 3. 填充 st->mode（目录/普通文件）与 st->size。
 * 4. 解锁返回 0。
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
 *
 * @param path 绝对路径
 * @return 目录句柄；失败返回 NULL
 *
 * @details
 * 1. 加锁，path_resolve 定位 ino；须为目录。
 * 2. 在 g_dirs[8] 中找 used==0 的槽，记录 ino 与 pos=0。
 * 3. 解锁返回句柄；目录无效或句柄池满返回 NULL。
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
 *
 * @param dir 目录句柄
 * @param out 输出 dirent（name/mode）
 * @return 1 成功读一项；0 EOF；-1 错误
 *
 * @details
 * 1. 校验 dir/out 与 dir.used；加锁。
 * 2. 若 pos+3 > d.size，返回 0（EOF）。
 * 3. 解析 dentry：child ino、nl、name；校验边界与 nl < MAX_NAME。
 * 4. 填充 out->name/out->mode，pos 前进 3+nl。
 * 5. 解锁返回 1。
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
 *
 * @param dir 目录句柄
 * @return 0 成功；-1 失败
 *
 * @details
 * 1. 校验 dir 非空；加锁。
 * 2. memset 清零 dir 结构，解锁返回 0。
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
