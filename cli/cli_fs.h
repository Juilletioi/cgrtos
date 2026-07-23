/**
 * @file cli_fs.h
 * @brief CLI 文件系统命令（VFS 之上；可由 CONFIG_CLI_FS 裁剪）
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 */
#ifndef CLI_FS_H
#define CLI_FS_H

#include "../kernel/cgrtos.h"

#ifndef CONFIG_CLI_FS
#define CONFIG_CLI_FS 1
#endif

#if CONFIG_CLI_FS

/**
 * @brief 初始化本 CLI 任务的文件系统会话（CWD=/）
 * @details 按当前任务 ID 绑定会话；重复调用重置 CWD 为根。
 * @return 无
 * @retval 无
 * @note 在 cli_task 启动时调用一次
 * @warning 无
 * @attention ❌ ISR；❌ 不阻塞
 */
void cli_fs_session_init(void);

/**
 * @brief 尝试分发文件系统命令
 * @details 识别 pwd/cd/ls/cat/…；处理成功返回 1；非 FS 命令返回 0。
 * @param[in] line 已 trim 的命令行
 * @return 1=已处理；0=非本模块命令
 * @retval 1 已处理
 * @retval 0 未识别为 FS 命令
 * @note 所有文件操作走 vfs_*，不耦合 LittleFS/FatFS
 * @warning mkfs / rm -r 会交互确认；长 IO 可用 Ctrl-C 中止
 * @attention ❌ ISR；✅ 可能阻塞
 */
int cli_fs_try_handle(char *line);

/**
 * @brief 向帮助文本追加 FS 命令说明与示例
 * @details 由 cli_help 在 CONFIG_CLI_FS=1 时调用。
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞 printf
 */
void cli_fs_help(void);

#else

static inline void cli_fs_session_init(void) {}
static inline int cli_fs_try_handle(char *line)
{
    (void)line;
    return 0;
}
static inline void cli_fs_help(void) {}

#endif /* CONFIG_CLI_FS */

#endif /* CLI_FS_H */
