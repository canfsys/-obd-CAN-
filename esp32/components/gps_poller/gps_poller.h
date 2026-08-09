/**
 * @file gps_poller.h
 * @brief 4G 模块 GPS 轮询器
 *
 * 在 light sleep / deep sleep 的 3 分钟定时唤醒后,
 * 通过 UART1 与 4G 模块通信, 获取 GPS 定位数据和网络状态.
 *
 * 通信流程:
 *   1. 等待 "GPS=OK" (模块就绪)
 *   2. 发送 "STATUS?"
 *   3. 解析返回的 KEY=VALUE 数据
 *   4. 若 FIX=1 则更新 system_state_t 并发送 JSON
 *   5. 若 FIX=0 则等待 10 秒后重试
 *
 * 整个轮询窗口最长 2 分钟.
 */

#ifndef GPS_POLLER_H
#define GPS_POLLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 UART1 连接 4G 模块
 *
 * 配置 GPIO41(TX)/GPIO42(RX) 为 UART1, 波特率 115200.
 * 拉高 GPIO21 为 4G 模块供电.
 */
void gps_poller_uart_init(void);

/**
 * @brief 关闭 UART1, 拉低 4G 模块电源
 */
void gps_poller_uart_deinit(void);

/**
 * @brief 执行一次 GPS 轮询周期 (最长 2 分钟)
 *
 * 内部流程:
 *   1. 等待 "GPS=OK"
 *   2. 发送 "STATUS?"
 *   3. 解析 KEY=VALUE 数据
 *   4. FIX=1 → 更新 system_state_t, 发送 JSON, 10 秒后再查一次
 *   5. FIX=0 → 等待 10 秒重试
 *   6. 2 分钟超时或 FIX=1 两次更新后结束
 *
 * @return true=成功获取到有效 GPS 数据, false=超时或失败
 */
bool gps_poller_run_cycle(void);

#ifdef __cplusplus
}
#endif

#endif /* GPS_POLLER_H */