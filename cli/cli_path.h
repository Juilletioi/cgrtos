/**
 * @file cli_path.h
 * @brief CLI 路径规范化 / CWD 会话 / Tab 路径补全
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * CWD 会话表无锁：仅由单一 cli 任务访问（多 CLI 任务属未定义行为）。
 * 补全经 vfs_opendir/readdir，内部会短暂持有 g_fs_mtx；禁止在持锁期间 UART 阻塞。
 */
#ifndef CLI_PATH_H
#define CLI_PATH_H

#include "../kernel/cgrtos.h"

#ifndef CONFIG_CLI_FS
/**
 * @brief 默认启用 CLI 文件系统（与 cli_fs.h 一致）
 * @warning 无运行时副作用（编译期常量）
 */
#define CONFIG_CLI_FS 1
#endif

/**
 * @brief 规范化绝对路径最大长度（含 NUL）
 * @warning 无运行时副作用（编译期常量）
 */
#define CLI_FS_PATH_MAX 192

#if CONFIG_CLI_FS

/**
 * @brief 初始化当前任务 FS 会话并将 CWD 置为 /
 * @details 按任务 ID 绑定会话槽；表满则静默失败。
 * @return 无
 * @retval 无
 * @note 保护：g_sess 无锁，单 CLI 任务独占
 * @warning 多 CLI 任务并发写会话未定义
 * @attention ❌ ISR；❌ block/switch
 */
void cli_path_session_init(void);

/**
 * @brief 规范化绝对路径并折叠 . / ..
 * @details 拒绝控制字符与过长组件；结果始终以 / 开头。
 * @param[in]  in     输入绝对路径（须以 / 开头）
 * @param[out] out    输出缓冲
 * @param[in]  out_sz 容量
 * @return 0 成功；-1 非法
 * @retval 0  合法
 * @retval -1 非法/过长/空指针
 * @note 纯计算，不访问 FS
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int cli_path_normalize(const char *in, char *out, size_t out_sz);

/**
 * @brief 将用户路径解析为规范化绝对路径
 * @details 相对路径拼接当前任务 CWD；失败时可打印原因。
 * @param[in]  user   用户路径；空表示 CWD
 * @param[out] abs    输出绝对路径
 * @param[in]  abs_sz 容量
 * @param[in]  quiet  非 0 则失败不打印
 * @return 0 成功；-1 失败
 * @retval 0  成功
 * @retval -1 失败
 * @note 保护：读 g_sess 无锁（单任务）
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cli_path_resolve(const char *user, char *abs, size_t abs_sz, int quiet);

/**
 * @brief 取当前任务 CWD 副本
 * @details 从会话槽拷贝 cwd 到 out。
 * @param[out] out    缓冲
 * @param[in]  out_sz 容量
 * @return 0 成功；-1 无会话
 * @retval 0  成功
 * @retval -1 无会话/参数错
 * @note 保护：g_sess 无锁
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
int cli_path_getcwd(char *out, size_t out_sz);

/**
 * @brief 设置当前任务 CWD（须为已规范化绝对路径）
 * @details 写入会话槽；调用方须已验证路径为目录。
 * @param[in] abs 绝对路径
 * @return 0 成功；-1 失败
 * @retval 0  成功
 * @retval -1 失败
 * @note 保护：g_sess 无锁
 * @warning 调用方须已验证路径为目录
 * @attention ❌ ISR；❌ block/switch
 */
int cli_path_setcwd(const char *abs);

/**
 * @brief 对行缓冲光标处路径 token 做 Tab 补全
 * @details 单 Tab：唯一匹配补全（目录加 /）或多匹配最长公共前缀；双 Tab（list_all!=0）：列出匹配项（由调用方负责重绘提示符）。
 * @param[in,out] line      行缓冲（可写）
 * @param[in]     line_cap  容量（含 NUL）
 * @param[in,out] len       当前长度
 * @param[in,out] cursor    光标位置（0..len）
 * @param[in]     list_all  非 0=列出全部匹配
 * @return 1=行已修改；0=无变化；-1=错误（已打印）
 * @retval 1  已补全
 * @retval 0  无匹配或仅响铃
 * @retval -1 错误
 * @note 保护：vfs_* 内 g_fs_mtx；堆分配用 g_klock。顺序：不在持 FS 锁时 malloc。
 * @warning 持锁期间禁止 UART 阻塞；本函数在 readdir 循环外打印
 * @attention ❌ ISR；✅ block/switch
 */
int cli_path_complete(char *line, int line_cap, int *len, int *cursor, int list_all);

#else

/**
 * @brief CONFIG_CLI_FS=0：会话初始化占位
 * @details 无操作。
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static inline void cli_path_session_init(void) {}

/**
 * @brief CONFIG_CLI_FS=0：规范化占位，始终失败
 * @details 忽略参数并返回 -1。
 * @param[in]  in     忽略
 * @param[out] out    忽略
 * @param[in]  out_sz 忽略
 * @return 始终 -1
 * @retval -1 未启用
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static inline int cli_path_normalize(const char *in, char *out, size_t out_sz)
{
    (void)in;
    (void)out;
    (void)out_sz;
    return -1;
}

/**
 * @brief CONFIG_CLI_FS=0：路径解析占位，始终失败
 * @details 忽略参数并返回 -1。
 * @param[in]  user   忽略
 * @param[out] abs    忽略
 * @param[in]  abs_sz 忽略
 * @param[in]  quiet  忽略
 * @return 始终 -1
 * @retval -1 未启用
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static inline int cli_path_resolve(const char *user, char *abs, size_t abs_sz, int quiet)
{
    (void)user;
    (void)abs;
    (void)abs_sz;
    (void)quiet;
    return -1;
}

/**
 * @brief CONFIG_CLI_FS=0：getcwd 占位，始终失败
 * @details 忽略参数并返回 -1。
 * @param[out] out    忽略
 * @param[in]  out_sz 忽略
 * @return 始终 -1
 * @retval -1 未启用
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static inline int cli_path_getcwd(char *out, size_t out_sz)
{
    (void)out;
    (void)out_sz;
    return -1;
}

/**
 * @brief CONFIG_CLI_FS=0：setcwd 占位，始终失败
 * @details 忽略参数并返回 -1。
 * @param[in] abs 忽略
 * @return 始终 -1
 * @retval -1 未启用
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static inline int cli_path_setcwd(const char *abs)
{
    (void)abs;
    return -1;
}

/**
 * @brief CONFIG_CLI_FS=0：Tab 补全占位，无变化
 * @details 忽略参数并返回 0。
 * @param[in,out] line      忽略
 * @param[in]     line_cap  忽略
 * @param[in,out] len       忽略
 * @param[in,out] cursor    忽略
 * @param[in]     list_all  忽略
 * @return 始终 0
 * @retval 0 无变化
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static inline int cli_path_complete(char *line, int line_cap, int *len, int *cursor, int list_all)
{
    (void)line;
    (void)line_cap;
    (void)len;
    (void)cursor;
    (void)list_all;
    return 0;
}

#endif /* CONFIG_CLI_FS */

#endif /* CLI_PATH_H */
