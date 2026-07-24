/**
 * @file vfs.h
 * @brief VFS 虚拟文件系统层：挂载表与统一 open/read/write/close
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * 上层（含 CLI）只应调用本头文件中的 `vfs_*` API，禁止直接依赖
 * LittleFS / FatFS 等具体后端。当前内建后端为 RAM FS（`cgrtos_fs_*`）。
 * 通过 `CONFIG_USE_VFS` 裁剪；为 0 时本头提供直通宏到 `cgrtos_fs_*`。
 */
#ifndef CGRTOS_VFS_H
#define CGRTOS_VFS_H

#include "cgrtos.h"

#ifndef CONFIG_USE_VFS
#define CONFIG_USE_VFS 1
#endif

/** @brief 最大同时挂载点数 */
#define VFS_MAX_MOUNTS 4
/** @brief 挂载点路径最大长度（含 NUL） */
#define VFS_MP_MAX     64
/** @brief fstype 名最大长度（含 NUL） */
#define VFS_FSTYPE_MAX 16

/**
 * @brief 文件系统后端操作集（与具体介质解耦；内核内部表项，用户勿直接改写）
 */
typedef struct vfs_fs_ops {
    const char *name; ///< 类型名，如 "ram" / "littlefs" / "fat"
    int (*mount)(const char *mp, void *arg);              ///< 挂载
    int (*umount)(const char *mp);                        ///< 卸载
    int (*mkfs)(const char *mp, void *arg);               ///< 格式化
    int (*sync)(const char *mp);                          ///< 同步
    int (*statfs)(const char *mp, cgrtos_statfs_t *st);   ///< 容量查询
    int (*open)(const char *path, int flags);             ///< 打开文件
    int (*close)(int fd);                                 ///< 关闭 fd
    int (*read)(int fd, void *buf, size_t len);           ///< 读
    int (*write)(int fd, const void *buf, size_t len);    ///< 写
    long (*lseek)(int fd, long off, int whence);          ///< 定位
    int (*unlink)(const char *path);                      ///< 删文件
    int (*mkdir)(const char *path);                       ///< 建目录
    int (*rmdir)(const char *path);                       ///< 删空目录
    int (*rename)(const char *oldp, const char *newp);    ///< 重命名
    int (*stat)(const char *path, cgrtos_stat_t *st);     ///< 属性
    cgrtos_dir_t *(*opendir)(const char *path);           ///< 打开目录
    int (*readdir)(cgrtos_dir_t *dir, cgrtos_dirent_t *out); ///< 读目录项
    int (*closedir)(cgrtos_dir_t *dir);                   ///< 关闭目录
} vfs_fs_ops_t;

/**
 * @brief 挂载表项快照（供 df / mount 列示；用户只读）
 */
typedef struct vfs_mount_info {
    char fstype[VFS_FSTYPE_MAX]; ///< 文件系统类型名
    char mp[VFS_MP_MAX];         ///< 挂载点绝对路径
    uint8_t used;                ///< 1=占用
} vfs_mount_info_t;

#if CONFIG_USE_VFS

/**
 * @brief 初始化 VFS 并默认挂载 ram 于 `/`
 * @details 注册内建后端，将 RAM FS 挂到根；幂等。
 * @return 无
 * @retval 无
 * @note 由 `cgrtos_init` 在 `cgrtos_fs_init` 之后调用
 * @warning 重复调用安全
 * @attention ❌ 不允许在中断上下文调用；❌ 不会阻塞/引起调度（内部可能持 FS 锁）
 */
void vfs_init(void);

/**
 * @brief 挂载文件系统到路径
 * @details 按 fstype 查找后端 ops，调用其 mount，写入挂载表。
 * @param[in] fstype 类型名，如 "ram"；未知类型失败
 * @param[in] mp     挂载点绝对路径（须以 `/` 开头）
 * @param[in] arg    后端私有参数；可为 NULL
 * @return 0 成功；负错误码
 * @retval 0   成功
 * @retval -1  参数非法 / 表满 / 后端不支持
 * @note 当前仅内建 "ram"；littlefs/fat 保留名但返回不支持
 * @warning 覆盖已挂载点失败
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_mount(const char *fstype, const char *mp, void *arg);

/**
 * @brief 卸载挂载点
 * @details 调用后端 umount 后清除挂载表项；拒绝卸载忙碌根策略由后端决定。
 * @param[in] mp 挂载点
 * @return 0 成功；-1 失败
 * @retval 0  已卸载
 * @retval -1 未找到或后端拒绝
 * @note 无
 * @warning 卸载后该前缀下的打开 fd 行为未定义
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_umount(const char *mp);

/**
 * @brief 格式化指定挂载点后端
 * @details 路由到对应 ops->mkfs。
 * @param[in] mp  挂载点或卷标识
 * @param[in] arg 后端参数；可为 NULL
 * @return 0 成功；-1 失败
 * @retval 0  已格式化
 * @retval -1 失败
 * @note CLI 须交互确认
 * @warning 销毁数据
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_mkfs(const char *mp, void *arg);

/**
 * @brief 同步全部或指定挂载点
 * @details mp 为 NULL 时同步所有挂载；否则仅同步匹配项。
 * @param[in] mp 挂载点；NULL=全部
 * @return 0 成功；-1 有失败
 * @retval 0  全部成功
 * @retval -1 至少一处失败
 * @note RAM 后端为空操作屏障
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_sync(const char *mp);

/**
 * @brief 查询挂载点容量
 * @details 路由到后端 statfs。
 * @param[in]  mp 挂载点
 * @param[out] st 输出
 * @return 0 成功；-1 失败
 * @retval 0  已填充
 * @retval -1 参数/挂载无效
 * @note 供 df 使用
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_statfs(const char *mp, cgrtos_statfs_t *st);

/**
 * @brief 枚举挂载表
 * @details 将至多 max 项写入 out，返回实际数量。
 * @param[out] out 输出数组
 * @param[in]  max 数组容量
 * @return 写入条数
 * @retval >=0 条数
 * @note 无
 * @warning out 须可写
 * @attention ❌ 不允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
int vfs_list_mounts(vfs_mount_info_t *out, int max);

/**
 * @brief 通过 VFS 打开文件
 * @details 按路径最长前缀匹配挂载点，转调后端 open。
 * @param[in] path  绝对路径
 * @param[in] flags CGRTOS_O_* 标志
 * @return fd；失败 -1
 * @retval >=0 有效 fd
 * @retval -1  失败
 * @note 应用层统一入口
 * @warning 路径须已规范化
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_open(const char *path, int flags);

/**
 * @brief 关闭 VFS 文件描述符
 * @details 路由到对应后端 close。
 * @param[in] fd 文件描述符
 * @return 0 成功；-1 失败
 * @retval 0  已关闭
 * @retval -1 非法 fd
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_close(int fd);

/**
 * @brief 读文件
 * @details 路由到后端 read。
 * @param[in]  fd  文件描述符
 * @param[out] buf 缓冲
 * @param[in]  len 长度
 * @return 字节数或 -1
 * @retval >=0 实际读取
 * @retval -1  错误
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_read(int fd, void *buf, size_t len);

/**
 * @brief 写文件
 * @details 路由到后端 write。
 * @param[in] fd  文件描述符
 * @param[in] buf 数据
 * @param[in] len 长度
 * @return 字节数或 -1
 * @retval >=0 实际写入
 * @retval -1  错误
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_write(int fd, const void *buf, size_t len);

/**
 * @brief 定位文件偏移
 * @details 路由到后端 lseek。
 * @param[in] fd     文件描述符
 * @param[in] off    偏移
 * @param[in] whence 0=SET 1=CUR 2=END
 * @return 新偏移或 -1
 * @retval >=0 新位置
 * @retval -1  错误
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
long vfs_lseek(int fd, long off, int whence);

/**
 * @brief 删除普通文件
 * @details 路由到后端 unlink。
 * @param[in] path 绝对路径
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_unlink(const char *path);

/**
 * @brief 创建目录
 * @details 路由到后端 mkdir。
 * @param[in] path 绝对路径
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_mkdir(const char *path);

/**
 * @brief 删除空目录
 * @details 路由到后端 rmdir。
 * @param[in] path 绝对路径
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_rmdir(const char *path);

/**
 * @brief 重命名/移动
 * @details 路由到后端 rename；通常须同挂载点。
 * @param[in] oldp 源路径
 * @param[in] newp 目标路径
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 须同挂载点
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_rename(const char *oldp, const char *newp);

/**
 * @brief 查询文件/目录属性
 * @details 路由到后端 stat。
 * @param[in]  path 绝对路径
 * @param[out] st   输出
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_stat(const char *path, cgrtos_stat_t *st);

/**
 * @brief 打开目录迭代
 * @details 路由到后端 opendir。
 * @param[in] path 绝对路径
 * @return 句柄或 NULL
 * @retval 非 NULL 成功
 * @retval NULL    失败
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
cgrtos_dir_t *vfs_opendir(const char *path);

/**
 * @brief 读取下一目录项
 * @details 路由到后端 readdir。
 * @param[in]  dir 目录句柄
 * @param[out] out 输出
 * @return 1=项 0=EOF -1=错
 * @retval 1  有项
 * @retval 0  结束
 * @retval -1 错误
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_readdir(cgrtos_dir_t *dir, cgrtos_dirent_t *out);

/**
 * @brief 关闭目录句柄
 * @details 路由到后端 closedir。
 * @param[in] dir 目录句柄
 * @return 0/-1
 * @retval 0  成功
 * @retval -1 失败
 * @note 无
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_closedir(cgrtos_dir_t *dir);

/**
 * @brief 查询打开句柄信息（供 fhandle）
 * @details 扫描内核 fd 表，输出 used 槽的简要信息到回调缓冲。
 * @param[out] buf     文本缓冲
 * @param[in]  buf_len 容量
 * @return 写入字符数（不含 NUL）
 * @retval >=0 长度
 * @note 仅报告 fd 占用与 flags；路径名不强制缓存
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；✅ 会阻塞/引起调度
 */
int vfs_dump_handles(char *buf, size_t buf_len);

#else /* !CONFIG_USE_VFS — 直通 RAM FS，保持 CLI 可编译裁剪 VFS */

/** @brief VFS 裁剪时空操作初始化 */
#define vfs_init()                ((void)0)
/**
 * @brief VFS 裁剪时挂载总失败
 * @param a fstype @param b mp @param c arg
 * @warning 参数可能被求值；恒返回 -1。
 */
#define vfs_mount(a, b, c)        (-1)
/**
 * @brief VFS 裁剪时卸载总失败
 * @param a 挂载点
 * @warning 参数可能被求值；恒返回 -1。
 */
#define vfs_umount(a)             (-1)
/**
 * @brief VFS 裁剪时格式化直通 RAM FS
 * @param a 忽略 @param b 忽略
 * @warning 参数可能被求值；实际调用 cgrtos_fs_format。
 */
#define vfs_mkfs(a, b)            cgrtos_fs_format()
/**
 * @brief VFS 裁剪时同步直通 RAM FS
 * @param a 忽略
 * @warning 参数可能被求值。
 */
#define vfs_sync(a)               cgrtos_fs_sync()
/**
 * @brief VFS 裁剪时 statfs 直通
 * @param a 忽略 @param b 输出结构
 * @warning 参数 a 可能被求值。
 */
#define vfs_statfs(a, b)          cgrtos_fs_statfs(b)
/**
 * @brief VFS 裁剪时无挂载表
 * @param a 输出 @param b 容量
 * @warning 参数可能被求值；恒返回 0。
 */
#define vfs_list_mounts(a, b)     (0)
/** @brief 打开文件（直通 cgrtos_fs_open） */
#define vfs_open                  cgrtos_fs_open
/** @brief 关闭文件（直通） */
#define vfs_close                 cgrtos_fs_close
/** @brief 读文件（直通） */
#define vfs_read                  cgrtos_fs_read
/** @brief 写文件（直通） */
#define vfs_write                 cgrtos_fs_write
/** @brief 定位（直通） */
#define vfs_lseek                 cgrtos_fs_lseek
/** @brief 删文件（直通） */
#define vfs_unlink                cgrtos_fs_unlink
/** @brief 建目录（直通） */
#define vfs_mkdir                 cgrtos_fs_mkdir
/** @brief 删目录（直通） */
#define vfs_rmdir                 cgrtos_fs_rmdir
/** @brief 重命名（直通） */
#define vfs_rename                cgrtos_fs_rename
/** @brief 属性（直通） */
#define vfs_stat                  cgrtos_fs_stat
/** @brief 打开目录（直通） */
#define vfs_opendir               cgrtos_fs_opendir
/** @brief 读目录（直通） */
#define vfs_readdir               cgrtos_fs_readdir
/** @brief 关闭目录（直通） */
#define vfs_closedir              cgrtos_fs_closedir
/**
 * @brief VFS 裁剪时句柄转储为空串
 * @details 写入空终止串并返回 0。
 * @param[out] buf 缓冲
 * @param[in]  buf_len 容量
 * @return 0
 * @retval 0 空结果
 * @note 无真实 fd 表可查
 * @warning 无
 * @attention ❌ 不允许在中断上下文调用；❌ 不会阻塞/引起调度
 */
static inline int vfs_dump_handles(char *buf, size_t buf_len)
{
    if (buf && buf_len) {
        buf[0] = 0;
    }
    return 0;
}

#endif /* CONFIG_USE_VFS */

#endif /* CGRTOS_VFS_H */
