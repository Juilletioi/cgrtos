/**
 * @file stress_cases.h
 * @brief 全 feature 并发压力测试（可供 CLI / APP=stress 调用）
 */
#ifndef STRESS_CASES_H
#define STRESS_CASES_H

/**
 * @brief 运行一轮完整压力测试（阻塞至结束）。
 * @note 打印 STRESS_PASSED / STRESS_FAILED；可重复调用。
 * @return 0 全部通过；非 0 有失败项。
 */
int stress_run(void);

int stress_pass_count(void);
int stress_fail_count(void);

#endif
