/**
 * @file stress_cases.h
 * @brief 全 feature 并发压力测试对外接口
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * 供 APP=stress / CLI `run stress` 共享。实现见 stress_cases.c。
 */

#ifndef STRESS_CASES_H
#define STRESS_CASES_H

/**
 * @brief 运行一轮完整压力测试（阻塞至结束）
 * @details 并发驱动调度、IPC、内存、流缓冲等子场景；结束时打印 STRESS_PASSED / STRESS_FAILED。
 * @return 0 全部通过；非 0 有失败项
 * @retval 0  成功
 * @retval !=0 至少一项失败
 * @note 可重复调用；计数器在 run 内重置
 * @warning 耗时长、占用多任务；勿在 ISR 调用
 * @attention ❌ ISR；✅ 任务上下文、大量阻塞
 */
int stress_run(void);

/**
 * @brief 读取最近一次 stress_run 的通过计数
 * @details 返回内部累计的成功子项数；run 开始前会清零。
 * @return 通过次数（非负）
 * @retval >=0 累计通过数
 * @note 只读；与 stress_fail_count 配对使用
 * @warning 无原子保护；并发读可能不一致
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int stress_pass_count(void);

/**
 * @brief 读取最近一次 stress_run 的失败计数
 * @details 返回内部累计的失败子项数；run 开始前会清零。
 * @return 失败次数（非负）
 * @retval >=0 累计失败数
 * @note 只读；非 0 时 stress_run 返回非 0
 * @warning 无原子保护；并发读可能不一致
 * @attention ✅ ISR 可读；❌ 不阻塞
 */
int stress_fail_count(void);

#endif
