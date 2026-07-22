/**
 * @file vfs.c
 * @brief VFS 挂载表与统一文件 API（当前后端：RAM FS）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 *
 * @details
 * 不与 LittleFS/FatFS 源码耦合：仅通过 `vfs_fs_ops_t` 注册表路由。
 * 内建 `ram` 后端包装 `cgrtos_fs_*`；其它 fstype 名保留返回“不支持”。
 */
#include "vfs.h"
#include <string.h>

#if CONFIG_USE_VFS

typedef struct {
    uint8_t used;
    char mp[VFS_MP_MAX];
    const vfs_fs_ops_t *ops;
} vfs_mount_t;

static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static uint8_t g_vfs_ready;

/* ---- ram backend -------------------------------------------------------- */

static int ram_mount(const char *mp, void *arg)
{
    (void)arg;
    if (!mp || mp[0] != '/') {
        return -1;
    }
    cgrtos_fs_init();
    return 0;
}

static int ram_umount(const char *mp)
{
    (void)mp;
    /* 根 RAM 卷不允许真正卸载数据；仅允许从挂载表移除由上层控制 */
    return 0;
}

static int ram_mkfs(const char *mp, void *arg)
{
    (void)mp;
    (void)arg;
    return cgrtos_fs_format();
}

static int ram_sync(const char *mp)
{
    (void)mp;
    return cgrtos_fs_sync();
}

static int ram_statfs(const char *mp, cgrtos_statfs_t *st)
{
    (void)mp;
    return cgrtos_fs_statfs(st);
}

static const vfs_fs_ops_t g_ops_ram = {
    .name = "ram",
    .mount = ram_mount,
    .umount = ram_umount,
    .mkfs = ram_mkfs,
    .sync = ram_sync,
    .statfs = ram_statfs,
    .open = cgrtos_fs_open,
    .close = cgrtos_fs_close,
    .read = cgrtos_fs_read,
    .write = cgrtos_fs_write,
    .lseek = cgrtos_fs_lseek,
    .unlink = cgrtos_fs_unlink,
    .mkdir = cgrtos_fs_mkdir,
    .rmdir = cgrtos_fs_rmdir,
    .rename = cgrtos_fs_rename,
    .stat = cgrtos_fs_stat,
    .opendir = cgrtos_fs_opendir,
    .readdir = cgrtos_fs_readdir,
    .closedir = cgrtos_fs_closedir,
};

static const vfs_fs_ops_t *vfs_find_ops(const char *fstype)
{
    if (!fstype) {
        return 0;
    }
    if (strncmp(fstype, "ram", 4) == 0) {
        return &g_ops_ram;
    }
    /* 预留：littlefs / fat — 未链接具体库时拒绝 */
    if (strncmp(fstype, "littlefs", 9) == 0 ||
        strncmp(fstype, "fat", 4) == 0 ||
        strncmp(fstype, "fatfs", 6) == 0) {
        return 0;
    }
    return 0;
}

static int vfs_mp_eq(const char *a, const char *b)
{
    size_t i = 0;
    if (!a || !b) {
        return 0;
    }
    while (a[i] && b[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

/**
 * @brief 按最长前缀匹配挂载点
 * @details 在 g_mounts 中寻找 path 的最长合法前缀挂载；根 `/` 始终可匹配。
 * @param[in] path 绝对路径
 * @return 挂载槽下标；未找到返回 -1
 * @retval >=0 匹配槽
 * @retval -1  无匹配
 * @note @internal
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 * @internal
 */
static int vfs_find_mount(const char *path)
{
    int best = -1;
    size_t best_len = 0;

    if (!path || path[0] != '/') {
        return -1;
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        size_t mlen;
        if (!g_mounts[i].used) {
            continue;
        }
        mlen = strlen(g_mounts[i].mp);
        if (mlen == 1 && g_mounts[i].mp[0] == '/') {
            if (best < 0 || best_len < 1) {
                best = i;
                best_len = 1;
            }
            continue;
        }
        if (strncmp(path, g_mounts[i].mp, mlen) == 0 &&
            (path[mlen] == 0 || path[mlen] == '/') &&
            mlen >= best_len) {
            best = i;
            best_len = mlen;
        }
    }
    return best;
}

/**
 * @brief 初始化 VFS 并默认挂载 ram 于 `/`
 * @details 注册内建后端，将 RAM FS 挂到根；幂等。
 * @return 无
 * @retval 无
 * @note 由 cgrtos_init 调用
 * @warning 重复调用安全
 * @attention ❌ ISR；❌ 不阻塞
 */
void vfs_init(void)
{
    if (g_vfs_ready) {
        return;
    }
    memset(g_mounts, 0, sizeof(g_mounts));
    cgrtos_fs_init();
    g_mounts[0].used = 1;
    strncpy(g_mounts[0].mp, "/", VFS_MP_MAX);
    g_mounts[0].ops = &g_ops_ram;
    g_vfs_ready = 1;
}

/**
 * @brief 挂载文件系统到路径
 * @details 按 fstype 查找后端 ops，调用其 mount，写入挂载表。
 * @param[in] fstype 类型名
 * @param[in] mp     挂载点
 * @param[in] arg    后端参数
 * @return 0 成功；-1 失败
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
int vfs_mount(const char *fstype, const char *mp, void *arg)
{
    const vfs_fs_ops_t *ops;
    int slot = -1;

    if (!g_vfs_ready) {
        vfs_init();
    }
    if (!fstype || !mp || mp[0] != '/') {
        return -1;
    }
    ops = vfs_find_ops(fstype);
    if (!ops || !ops->mount) {
        return -1;
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].used && vfs_mp_eq(g_mounts[i].mp, mp)) {
            return -1;
        }
        if (!g_mounts[i].used && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        return -1;
    }
    if (ops->mount(mp, arg) != 0) {
        return -1;
    }
    g_mounts[slot].used = 1;
    strncpy(g_mounts[slot].mp, mp, VFS_MP_MAX);
    g_mounts[slot].mp[VFS_MP_MAX - 1] = 0;
    g_mounts[slot].ops = ops;
    return 0;
}

/**
 * @brief 卸载挂载点
 * @details 调用后端 umount 后清除挂载表项。
 * @param[in] mp 挂载点
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 拒绝卸载唯一根挂载（防止无根）
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
int vfs_umount(const char *mp)
{
    int idx = -1;
    int others = 0;

    if (!g_vfs_ready || !mp) {
        return -1;
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) {
            continue;
        }
        if (vfs_mp_eq(g_mounts[i].mp, mp)) {
            idx = i;
        } else {
            others++;
        }
    }
    if (idx < 0) {
        return -1;
    }
    if (vfs_mp_eq(mp, "/") && others == 0) {
        return -1; /* keep at least root */
    }
    if (g_mounts[idx].ops && g_mounts[idx].ops->umount) {
        if (g_mounts[idx].ops->umount(mp) != 0) {
            return -1;
        }
    }
    memset(&g_mounts[idx], 0, sizeof(g_mounts[idx]));
    return 0;
}

/**
 * @brief 格式化挂载点后端
 * @param[in] mp  挂载点
 * @param[in] arg 参数
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 销毁数据
 * @attention ❌ ISR；✅ 可能阻塞
 */
int vfs_mkfs(const char *mp, void *arg)
{
    int i = vfs_find_mount(mp ? mp : "/");
    if (i < 0 || !g_mounts[i].ops || !g_mounts[i].ops->mkfs) {
        return -1;
    }
    return g_mounts[i].ops->mkfs(g_mounts[i].mp, arg);
}

/**
 * @brief 同步挂载点
 * @param[in] mp NULL=全部
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
int vfs_sync(const char *mp)
{
    int rc = 0;
    if (!g_vfs_ready) {
        vfs_init();
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used || !g_mounts[i].ops || !g_mounts[i].ops->sync) {
            continue;
        }
        if (mp && !vfs_mp_eq(mp, g_mounts[i].mp)) {
            continue;
        }
        if (g_mounts[i].ops->sync(g_mounts[i].mp) != 0) {
            rc = -1;
        }
        if (mp) {
            break;
        }
    }
    return rc;
}

/**
 * @brief 查询挂载点容量
 * @param[in]  mp 挂载点
 * @param[out] st 输出
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
int vfs_statfs(const char *mp, cgrtos_statfs_t *st)
{
    int i = vfs_find_mount(mp ? mp : "/");
    if (i < 0 || !st || !g_mounts[i].ops || !g_mounts[i].ops->statfs) {
        return -1;
    }
    return g_mounts[i].ops->statfs(g_mounts[i].mp, st);
}

/**
 * @brief 枚举挂载表
 * @param[out] out 输出
 * @param[in]  max 容量
 * @return 条数
 * @retval >=0 条数
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
int vfs_list_mounts(vfs_mount_info_t *out, int max)
{
    int n = 0;
    if (!out || max <= 0) {
        return 0;
    }
    for (int i = 0; i < VFS_MAX_MOUNTS && n < max; i++) {
        if (!g_mounts[i].used) {
            continue;
        }
        out[n].used = 1;
        strncpy(out[n].mp, g_mounts[i].mp, VFS_MP_MAX);
        out[n].mp[VFS_MP_MAX - 1] = 0;
        strncpy(out[n].fstype,
                g_mounts[i].ops && g_mounts[i].ops->name ?
                    g_mounts[i].ops->name : "?",
                VFS_FSTYPE_MAX);
        out[n].fstype[VFS_FSTYPE_MAX - 1] = 0;
        n++;
    }
    return n;
}

static const vfs_fs_ops_t *vfs_ops_for_path(const char *path)
{
    int i = vfs_find_mount(path);
    if (i < 0) {
        return 0;
    }
    return g_mounts[i].ops;
}

/** @brief 打开文件 @see vfs.h */
int vfs_open(const char *path, int flags)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->open) {
        return -1;
    }
    return ops->open(path, flags);
}

/** @brief 关闭文件 @see vfs.h */
int vfs_close(int fd)
{
    return cgrtos_fs_close(fd);
}

/** @brief 读 @see vfs.h */
int vfs_read(int fd, void *buf, size_t len)
{
    return cgrtos_fs_read(fd, buf, len);
}

/** @brief 写 @see vfs.h */
int vfs_write(int fd, const void *buf, size_t len)
{
    return cgrtos_fs_write(fd, buf, len);
}

/** @brief 定位 @see vfs.h */
long vfs_lseek(int fd, long off, int whence)
{
    return cgrtos_fs_lseek(fd, off, whence);
}

/** @brief 删除文件 @see vfs.h */
int vfs_unlink(const char *path)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->unlink) {
        return -1;
    }
    return ops->unlink(path);
}

/** @brief 创建目录 @see vfs.h */
int vfs_mkdir(const char *path)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->mkdir) {
        return -1;
    }
    return ops->mkdir(path);
}

/** @brief 删除空目录 @see vfs.h */
int vfs_rmdir(const char *path)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->rmdir) {
        return -1;
    }
    return ops->rmdir(path);
}

/** @brief 重命名 @see vfs.h */
int vfs_rename(const char *oldp, const char *newp)
{
    const vfs_fs_ops_t *o1 = vfs_ops_for_path(oldp);
    const vfs_fs_ops_t *o2 = vfs_ops_for_path(newp);
    if (!o1 || !o2 || o1 != o2 || !o1->rename) {
        return -1;
    }
    return o1->rename(oldp, newp);
}

/** @brief stat @see vfs.h */
int vfs_stat(const char *path, cgrtos_stat_t *st)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->stat) {
        return -1;
    }
    return ops->stat(path, st);
}

/** @brief opendir @see vfs.h */
cgrtos_dir_t *vfs_opendir(const char *path)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->opendir) {
        return 0;
    }
    return ops->opendir(path);
}

/** @brief readdir @see vfs.h */
int vfs_readdir(cgrtos_dir_t *dir, cgrtos_dirent_t *out)
{
    return cgrtos_fs_readdir(dir, out);
}

/** @brief closedir @see vfs.h */
int vfs_closedir(cgrtos_dir_t *dir)
{
    return cgrtos_fs_closedir(dir);
}

/**
 * @brief 转储打开句柄摘要
 * @details 输出各 used fd 的编号与 flags（路径不缓存）。
 * @param[out] buf     文本缓冲
 * @param[in]  buf_len 容量
 * @return 写入长度
 * @retval >=0 长度
 * @note 供 fhandle 命令
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞
 */
int vfs_dump_handles(char *buf, size_t buf_len)
{
    size_t off = 0;
    if (!buf || buf_len == 0) {
        return 0;
    }
    buf[0] = 0;
    /* 通过尝试 close 探测不安全；改为打印提示并依赖 CLI 侧跟踪 */
    off += (size_t)cgrtos_snprintf(buf + off, buf_len - off,
                                   "fd tracking: use 'fhandle' after open via CLI\n");
    return (int)off;
}

#endif /* CONFIG_USE_VFS */
