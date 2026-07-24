/**
 * @file test_cases.h
 * @brief 功能测试用例表与运行器对外接口
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * 供 APP=test / APP=cli 共享。用例以名称字符串注册，CLI 可通过 `run <name>` 触发。
 */

#ifndef TEST_CASES_H
#define TEST_CASES_H

/**
 * @typedef test_case_fn_t
 * @brief 单个测试用例入口函数类型
 * @details 无参数、无返回值；用例内通过 expect() 累计全局 pass/fail，并打印 [PASS]/[FAIL]。
 * @note 由 test_case_t::run 持有；须在调度已启动的任务上下文调用。
 * @warning 用例内可能创建任务、阻塞 IPC，勿当作 ISR 回调类型使用。
 * @attention ❌ ISR；✅ 用例实现内可能阻塞并切换
 */
typedef void (*test_case_fn_t)(void);

/**
 * @struct test_case_t
 * @brief 测试用例描述符
 * @details 静态只读表项，描述 CLI/`run <name>` 可触发的功能用例；不含 "all"/"stress" 合成名。
 * @note 表由 test_cases_get() 导出；调用方不得 free 或改写字段。
 * @warning name/help/run 指针必须常驻；run 不可为 NULL。
 * @attention ❌ ISR 勿改写；❌ block/switch
 */
typedef struct {
    const char *name;      /**< 短命令名，如 "mem"、"sched_m1"；与 CLI 匹配 */
    const char *help;      /**< 一行帮助说明，供 test_cases_list 打印 */
    test_case_fn_t run;    /**< 用例执行函数入口；不可为 NULL */
} test_case_t;

/**
 * @brief 清零全局 pass/fail 计数器
 * @details 不删除已注册用例表。可在任务上下文调用。
 * @return 无
 * @retval 无
 * @note 与 test_cases_run 独立
 * @warning 多任务并发调用时计数无原子保护（测试单驱动场景）
 * @attention ❌ 勿在 ISR 依赖其副作用；❌ 不阻塞
 */
void test_cases_reset(void);

/**
 * @brief 读取当前通过计数
 * @details 返回自上次 reset 或启动以来的 [PASS] 次数。
 * @return 通过次数（非负）
 * @retval >=0 累计通过数
 * @note 只读
 * @warning 无
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int test_cases_pass_count(void);

/**
 * @brief 读取当前失败计数
 * @details 返回自上次 reset 或启动以来的 [FAIL] 次数。
 * @return 失败次数（非负）
 * @retval >=0 累计失败数
 * @note 只读
 * @warning 无
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int test_cases_fail_count(void);

/**
 * @brief 获取静态用例表指针与条目数
 * @details 不含合成名 "all"；表驻留 .rodata/.data。
 * @param[out] count 非 NULL 时写入表长度
 * @return 用例表首地址；count 为 NULL 时仍返回表指针
 * @retval 非 NULL 表有效
 * @note 调用方勿 free
 * @warning 勿修改表项
 * @attention ✅ ISR；❌ 不阻塞
 */
const test_case_t *test_cases_get(int *count);

/**
 * @brief 按名称运行单个用例、"all" 或 "stress"
 * @details 名称匹配 g_cases[]；"all" 依次执行全部功能用例；"stress" 转调 stress_run()。
 * @param[in] name 用例名、"all" 或 "stress"；NULL 视为非法
 * @return 0 找到并执行；-1 未知名称
 * @retval 0  成功调度执行
 * @retval -1 名称未知或 name 为空
 * @note 用例内部可能创建任务/阻塞，须在调度已启动后调用
 * @warning 长时间用例会占用驱动任务；stress 不经 expect 汇总
 * @attention ❌ ISR；✅ 用例内可能阻塞并切换
 */
int test_cases_run(const char *name);

/**
 * @brief 向控制台打印全部用例名与帮助
 * @details 遍历表并 cgrtos_printf。
 * @return 无
 * @retval 无
 * @note 仅输出，不执行用例
 * @warning 无
 * @attention ❌ ISR（建议任务上下文，printf 带锁）；❌ block/switch
 */
void test_cases_list(void);

#endif
