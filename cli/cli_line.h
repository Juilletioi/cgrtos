/**
 * @file cli_line.h
 * @brief CLI 可复用行编辑（历史 / 光标 / Tab 补全钩子）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details 历史缓冲与草稿无锁，仅 cli 任务使用。
 *          Tab 调用 cli_path_complete（可能阻塞于 g_fs_mtx / 堆）。
 */
#ifndef CLI_LINE_H
#define CLI_LINE_H

#include "../kernel/cgrtos.h"

/**
 * @brief 行缓冲最大长度（含 NUL），与 CLI_FS_PATH_MAX 对齐
 * @warning 无运行时副作用（编译期常量）
 */
#define CLI_LINE_MAX  192
/**
 * @brief 命令历史条数上限（环形缓冲）
 * @warning 无运行时副作用（编译期常量）
 */
#define CLI_HIST_MAX  16
/**
 * @brief 默认提示符字符串
 * @warning 无运行时副作用（编译期常量）
 */
#define CLI_PROMPT_DEFAULT "cgrtos> "

/**
 * @brief 从 UART 读入一行（可编辑）
 * @details 支持左右光标、Backspace/Delete、↑↓ 历史、Tab 路径补全、Ctrl-C 清空。Enter 提交；返回后 line 以 NUL 结尾。
 * @param[out] line     输出缓冲
 * @param[in]  line_cap 容量（须 >= 2，建议 CLI_LINE_MAX）
 * @param[in]  prompt   提示符；NULL 用默认
 * @return 0 正常提交；1 Ctrl-C 清空后空行提交风格（调用方可忽略）
 * @retval 0 正常
 * @retval 1 用户 Ctrl-C（line 为空）
 * @note 保护：历史表无锁单任务；Tab→cli_path_complete 见其文档
 * @warning 吞掉 ANSI CSI；与 vi 接管互斥（同任务）
 * @attention ❌ ISR；✅ block/switch
 */
int cli_line_read(char *line, int line_cap, const char *prompt);

/**
 * @brief 将已提交行推入历史（空行忽略）
 * @details 写入历史环；空串或 NULL 直接返回。
 * @param[in] line 行
 * @return 无
 * @retval 无
 * @note 保护：g_hist 无锁单任务
 * @warning 无
 * @attention ❌ ISR；❌ block/switch
 */
void cli_line_hist_push(const char *line);

#endif /* CLI_LINE_H */
