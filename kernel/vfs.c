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

/**
 * @brief RAM FS 后端挂载回调
 * @details 校验挂载点为绝对路径后调用 cgrtos_fs_init；arg 忽略。
 * @param[in] mp  挂载点路径
 * @param[in] arg 未使用
 * @return 0 成功；-1 参数非法
 * @retval 0  成功
 * @retval -1 mp 非法
 * @note 由 vfs_mount / vfs_init 经 ops 表调用
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static int ram_mount(const char *mp, void *arg)
{
    (void)arg;
    if (!mp || mp[0] != '/') {
        return -1;
    }
    cgrtos_fs_init();
    return 0;
}

/**
 * @brief RAM FS 后端卸载回调
 * @details 根 RAM 卷不销毁数据，仅允许上层从挂载表移除；始终返回 0。
 * @param[in] mp 挂载点（未使用）
 * @return 0
 * @retval 0 成功
 * @note 与 vfs_umount 配合
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static int ram_umount(const char *mp)
{
    (void)mp;
    /* 根 RAM 卷不允许真正卸载数据；仅允许从挂载表移除由上层控制 */
    return 0;
}

/**
 * @brief RAM FS 后端格式化回调
 * @details 委托 cgrtos_fs_format。
 * @param[in] mp  挂载点（未使用）
 * @param[in] arg 未使用
 * @return cgrtos_fs_format 返回值
 * @retval 0  成功
 * @retval -1 失败
 * @note 销毁 RAM 卷内容
 * @warning 不可恢复
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static int ram_mkfs(const char *mp, void *arg)
{
    (void)mp;
    (void)arg;
    return cgrtos_fs_format();
}

/**
 * @brief RAM FS 后端同步回调
 * @details 委托 cgrtos_fs_sync。
 * @param[in] mp 挂载点（未使用）
 * @return cgrtos_fs_sync 返回值
 * @retval 0  成功
 * @retval -1 失败
 * @note RAM 后端多为空操作
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 * @internal
 */
static int ram_sync(const char *mp)
{
    (void)mp;
    return cgrtos_fs_sync();
}

/**
 * @brief RAM FS 后端 statfs 回调
 * @details 委托 cgrtos_fs_statfs 填充容量信息。
 * @param[in]  mp 挂载点（未使用）
 * @param[out] st 输出结构
 * @return cgrtos_fs_statfs 返回值
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning st 不可为 NULL（由后端校验）
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
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

/**
 * @brief 按 fstype 名查找文件系统 ops
 * @details 当前仅支持 "ram"；littlefs/fat(fs) 名保留但未链接时返回 NULL。
 * @param[in] fstype 类型名
 * @return ops 指针；不支持或参数非法返回 NULL
 * @retval 非 NULL 找到
 * @retval NULL    不支持
 * @note 扩展后端时在此注册
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
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

/**
 * @brief 比较两个挂载点路径是否全等
 * @details 逐字符比较直至双端结束符，要求完全一致。
 * @param[in] a 路径 A
 * @param[in] b 路径 B
 * @return 1 相等；0 不等或空指针
 * @retval 1 相等
 * @retval 0 不等
 * @note 不做规范化（无去重斜杠）
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
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
 * @note 路径须以 '/' 开头
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
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
 * @attention ❌ ISR；❌ block/switch
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
 * @attention ❌ ISR；✅ block/switch
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
 * @attention ❌ ISR；✅ block/switch
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
 * @details 按路径找挂载，调用后端 mkfs；销毁该卷数据。
 * @param[in] mp  挂载点
 * @param[in] arg 后端参数
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 销毁数据
 * @attention ❌ ISR；✅ block/switch
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
 * @details mp 非空则同步匹配挂载；NULL 则同步全部已用挂载。
 * @param[in] mp NULL=全部
 * @return 0/-1（任一路失败则 -1）
 * @retval 0  成功
 * @retval -1 失败
 * @note 未就绪时先 vfs_init
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
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
 * @details 最长前缀匹配后调用后端 statfs。
 * @param[in]  mp 挂载点
 * @param[out] st 输出
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note mp 为空时按 `/` 匹配
 * @warning st 不可为 NULL
 * @attention ❌ ISR；✅ block/switch
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
 * @details 将已用挂载的 mp/fstype 写入 out，最多 max 条。
 * @param[out] out 输出
 * @param[in]  max 容量
 * @return 条数
 * @retval >=0 条数
 * @note out/max 非法返回 0
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
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

/**
 * @brief 按路径解析所属文件系统 ops
 * @details 调用 vfs_find_mount 后返回对应 ops。
 * @param[in] path 绝对路径
 * @return ops 指针；未挂载返回 NULL
 * @retval 非 NULL 找到
 * @retval NULL    无匹配
 * @note 供 open/unlink 等路径类 API 复用
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static const vfs_fs_ops_t *vfs_ops_for_path(const char *path)
{
    int i = vfs_find_mount(path);
    if (i < 0) {
        return 0;
    }
    return g_mounts[i].ops;
}

/**
 * @brief 打开文件
 * @details 按路径选后端并调用 ops->open。
 * @param[in] path  绝对路径
 * @param[in] flags 打开标志
 * @return 文件描述符；失败 -1
 * @retval >=0 fd
 * @retval -1  失败
 * @note 无匹配挂载或无 open 则失败
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_open(const char *path, int flags)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->open) {
        return -1;
    }
    return ops->open(path, flags);
}

/**
 * @brief 关闭文件
 * @details 委托 cgrtos_fs_close（当前后端 fd 全局）。
 * @param[in] fd 文件描述符
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_close(int fd)
{
    return cgrtos_fs_close(fd);
}

/**
 * @brief 读文件
 * @details 委托 cgrtos_fs_read。
 * @param[in]  fd  文件描述符
 * @param[out] buf 缓冲
 * @param[in]  len 长度
 * @return 实际字节数；失败 -1
 * @retval >=0 字节数
 * @retval -1  失败
 * @note 无
 * @warning buf 须可写
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_read(int fd, void *buf, size_t len)
{
    return cgrtos_fs_read(fd, buf, len);
}

/**
 * @brief 写文件
 * @details 委托 cgrtos_fs_write。
 * @param[in] fd  文件描述符
 * @param[in] buf 数据
 * @param[in] len 长度
 * @return 实际字节数；失败 -1
 * @retval >=0 字节数
 * @retval -1  失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_write(int fd, const void *buf, size_t len)
{
    return cgrtos_fs_write(fd, buf, len);
}

/**
 * @brief 文件定位
 * @details 委托 cgrtos_fs_lseek。
 * @param[in] fd     文件描述符
 * @param[in] off    偏移
 * @param[in] whence SEEK 起点
 * @return 新偏移；失败 -1
 * @retval >=0 偏移
 * @retval -1  失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
long vfs_lseek(int fd, long off, int whence)
{
    return cgrtos_fs_lseek(fd, off, whence);
}

/**
 * @brief 删除文件
 * @details 按路径选后端并调用 ops->unlink。
 * @param[in] path 绝对路径
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_unlink(const char *path)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->unlink) {
        return -1;
    }
    return ops->unlink(path);
}

/**
 * @brief 创建目录
 * @details 按路径选后端并调用 ops->mkdir。
 * @param[in] path 绝对路径
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_mkdir(const char *path)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->mkdir) {
        return -1;
    }
    return ops->mkdir(path);
}

/**
 * @brief 删除空目录
 * @details 按路径选后端并调用 ops->rmdir。
 * @param[in] path 绝对路径
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 目录须为空
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_rmdir(const char *path)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->rmdir) {
        return -1;
    }
    return ops->rmdir(path);
}

/**
 * @brief 重命名
 * @details 要求 old/new 落在同一挂载，再调用 ops->rename。
 * @param[in] oldp 原路径
 * @param[in] newp 新路径
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败（跨挂载或不支持）
 * @note 跨卷 rename 拒绝
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_rename(const char *oldp, const char *newp)
{
    const vfs_fs_ops_t *o1 = vfs_ops_for_path(oldp);
    const vfs_fs_ops_t *o2 = vfs_ops_for_path(newp);
    if (!o1 || !o2 || o1 != o2 || !o1->rename) {
        return -1;
    }
    return o1->rename(oldp, newp);
}

/**
 * @brief 查询文件/目录属性
 * @details 按路径选后端并调用 ops->stat。
 * @param[in]  path 绝对路径
 * @param[out] st   输出
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning st 不可为 NULL
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_stat(const char *path, cgrtos_stat_t *st)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->stat) {
        return -1;
    }
    return ops->stat(path, st);
}

/**
 * @brief 打开目录
 * @details 按路径选后端并调用 ops->opendir。
 * @param[in] path 绝对路径
 * @return 目录句柄；失败 NULL
 * @retval 非 NULL 成功
 * @retval NULL    失败
 * @note 须配对 vfs_closedir
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
cgrtos_dir_t *vfs_opendir(const char *path)
{
    const vfs_fs_ops_t *ops = vfs_ops_for_path(path);
    if (!ops || !ops->opendir) {
        return 0;
    }
    return ops->opendir(path);
}

/**
 * @brief 读取目录项
 * @details 委托 cgrtos_fs_readdir。
 * @param[in]  dir 目录句柄
 * @param[out] out 目录项
 * @return 1 有项；0 结束；-1 失败
 * @retval 1  有项
 * @retval 0  结束
 * @retval -1 失败
 * @note 无
 * @warning out 不可为 NULL
 * @attention ❌ ISR；✅ block/switch
 */
int vfs_readdir(cgrtos_dir_t *dir, cgrtos_dirent_t *out)
{
    return cgrtos_fs_readdir(dir, out);
}

/**
 * @brief 关闭目录
 * @details 委托 cgrtos_fs_closedir。
 * @param[in] dir 目录句柄
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
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
 * @attention ❌ ISR；✅ block/switch
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
