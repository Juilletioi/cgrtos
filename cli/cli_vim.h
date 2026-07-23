/**
 * @file cli_vim.h
 * @brief CLI POSIX vi 风格编辑器（CONFIG_CLI_VIM）
 * @author Cong Zhou / Juilletioi
 * @version 5.1.0
 * @date 2026-07-23
 * @copyright CG-RTOS
 *
 * @details 缓冲/undo/剪贴板仅 cli 任务访问；FS 经 vfs_*（g_fs_mtx）。
 *          详见 docs/MODULE_CLI_VIM.md。
 */
#ifndef CLI_VIM_H
#define CLI_VIM_H

#include "../kernel/cgrtos.h"

#ifndef CONFIG_CLI_VIM
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
 * @attention ❌ ISR；✅ 可能阻塞
 */
int cli_vim_try_handle(char *line);

/**
 * @brief 向 help 追加 vim 说明
 * @return 无
 * @retval 无
 * @note 无
 * @warning 无
 * @attention ❌ ISR；✅ 可能阻塞 printf
 */
void cli_vim_help(void);

#else

static inline int cli_vim_try_handle(char *line)
{
    (void)line;
    return 0;
}
static inline void cli_vim_help(void) {}

#endif

#endif /* CLI_VIM_H */
