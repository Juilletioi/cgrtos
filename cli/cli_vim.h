/**
 * @file cli_vim.h
 * @brief CLI POSIX vi 风格编辑器（CONFIG_CLI_VIM）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details 缓冲/undo/剪贴板仅 cli 任务访问；FS 经 vfs_*（g_fs_mtx）。
 *          详见 docs/MODULE_CLI_VIM.md。
 */
#ifndef CLI_VIM_H
#define CLI_VIM_H

#include "../kernel/cgrtos.h"

#ifndef CONFIG_CLI_VIM
/**
 * @brief 默认关闭 CLI vi 编辑器（需与 CONFIG_CLI_FS 同开）
 * @warning 无运行时副作用（编译期常量）
 */
#define CONFIG_CLI_VIM 0
#endif

#if CONFIG_CLI_VIM && CONFIG_CLI_FS

/**
 * @brief 尝试分发 vi/vim/edit 命令并进入编辑器
 * @details 识别 `vi`/`vim`/`edit` [path]；接管 UART 直至退出。
 * @param[in] line 已 trim 命令行
 * @return 1=已处理；0=非本命令
 * @retval 1 已处理
 * @retval 0 未识别
 * @note 保护：编辑器状态单任务独占；load/save 短持 g_fs_mtx
 * @warning 脏缓冲 :q 拒绝；禁止在持 FS 锁时阻塞 UART
 * @attention ❌ ISR；✅ block/switch
 */
int cli_vim_try_handle(char *line);

/**
 * @brief 向 help 追加 vim 说明
 * @details 打印一行 vi|vim|edit 用法提示。
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ block/switch
 */
void cli_vim_help(void);

#else

/**
 * @brief CONFIG_CLI_VIM=0：不处理 vi 命令
 * @details 始终返回 0。
 * @param[in] line 命令行（忽略）
 * @return 始终 0
 * @retval 0 未处理
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static inline int cli_vim_try_handle(char *line)
{
    (void)line;
    return 0;
}

/**
 * @brief CONFIG_CLI_VIM=0：不追加 vim 帮助
 * @details 无操作。
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 * @internal
 */
static inline void cli_vim_help(void) {}

#endif

#endif /* CLI_VIM_H */
