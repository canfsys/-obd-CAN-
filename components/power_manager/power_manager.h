/**
 * @file power_manager.h
 * @brief 电源管理模式管理器
 *
 * 三种模式:
 *   正常模式 (NORMAL) — 全速运行, 所有外设工作
 *   待机模式 (STANDBY) — 1 分钟无 CAN 报文进入 light sleep,
 *                        每 3 分钟 GPS 轮询, 10 分钟累积后进 deep sleep
 *   休眠模式 (SLEEP)   — deep sleep, 每 3 分钟 GPS 轮询
 *
 * 唤醒类型:
 *   自主唤醒 (定时器) → 仅 4G GPS 轮询, 不初始化外设
 *   外部唤醒 (GPIO1)  → 全初始化, 恢复正常模式
 */

#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_sleep.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 电源管理模式枚举 */
typedef enum {
    POWER_MODE_NORMAL  = 0,    /*!< 正常模式: 所有外设全开    */
    POWER_MODE_STANDBY,        /*!< 待机模式: CAN 休眠, light sleep */
    POWER_MODE_SLEEP,          /*!< 休眠模式: 4G 断电, deep sleep  */
} power_mode_t;

/** 待机空闲超时 (毫秒) */
#define STANDBY_IDLE_TIMEOUT_MS   (1 * 60 * 1000)   /* 1 分钟 */

/** GPS 轮询间隔 (微秒) */
#define GPS_POLL_INTERVAL_US      (3 * 60 * 1000000) /* 3 分钟 */

/** GPS 轮询窗口: 每次唤醒最长等待时间 (毫秒) */
#define GPS_POLL_WINDOW_MS        (2 * 60 * 1000)    /* 2 分钟 */

/** GPS 重试间隔 (毫秒) */
#define GPS_RETRY_INTERVAL_MS     10000               /* 10 秒 */

/** light sleep → deep sleep 累积超时 (微秒) */
#define LIGHT_SLEEP_TO_DEEP_US    (10 * 60 * 1000000) /* 10 分钟 */

/** 4G 模块 UART1 波特率 */
#define MODEM_UART_BAUD           115200

/**
 * @brief 初始化电源管理器
 */
void power_manager_init(void);

/**
 * @brief 标记 CAN 活动 (收到新报文时调用)
 *
 * 重置空闲计时器, 防止误入待机模式.
 */
void power_manager_mark_can_activity(void);

/**
 * @brief 获取当前电源模式
 *
 * @return power_mode_t
 */
power_mode_t power_manager_get_mode(void);

/**
 * @brief 设置当前电源模式
 *
 * @param mode 新模式
 */
void power_manager_set_mode(power_mode_t mode);

/**
 * @brief 每秒调用的节拍函数 (由 sensor_task 调用)
 *
 * 在正常模式下检查空闲超时, 超时则发起待机切换.
 *
 * @param now_ms  当前系统时间 (ms, esp_timer_get_time/1000)
 */
void power_manager_tick(uint64_t now_ms);

/**
 * @brief 检查是否需要进入待机模式
 *
 * @return true=应进入待机, false=继续正常
 */
bool power_manager_should_standby(void);

/**
 * @brief 获取自上次 CAN 活跃以来的空闲时长 (ms)
 *
 * @return uint64_t 空闲毫秒数
 */
uint64_t power_manager_get_idle_ms(void);

/**
 * @brief 获取唤醒原因
 *
 * @return esp_sleep_wakeup_cause_t
 */
esp_sleep_wakeup_cause_t power_manager_get_wakeup_cause(void);

/**
 * @brief 是否是自主唤醒 (RTC 定时器)
 *
 * @return true=自主唤醒, false=外部唤醒或上电
 */
bool power_manager_is_auto_wake(void);

/**
 * @brief 注册 SPI 设备句柄 (由 main.c 在 init_all 后调用)
 *
 * @param h_sit1  SIT1145-1 句柄
 * @param h_sit2  SIT1145-2 句柄
 * @param h_mcp   MCP2515 句柄
 */
void power_manager_register_spi_handles(spi_device_handle_t h_sit1,
                                        spi_device_handle_t h_sit2,
                                        spi_device_handle_t h_mcp);

/**
 * @brief 注册 SD 写入任务句柄
 *
 * @param task_handle SD 写入任务句柄
 */
void power_manager_register_sd_task(TaskHandle_t task_handle);

/**
 * @brief 进入 light sleep (待机模式)
 *
 * 设置 3 分钟 GPS 轮询定时器, 进入 light sleep.
 * 唤醒后自动处理 GPS 轮询逻辑, 然后根据情况决定返回正常模式还是继续 sleep.
 */
void power_manager_enter_standby(void);

/**
 * @brief 进入 deep sleep (休眠模式)
 *
 * 关闭 4G 电源, 全部引脚(除 GPIO1)高阻态,
 * 设置 3 分钟 GPS 轮询定时器, 然后进入 deep sleep.
 * 不会返回.
 */
void power_manager_enter_sleep(void);

/**
 * @brief GPS 轮询周期 (由 app_main 在自主唤醒时调用)
 *
 * 内部包含 2 分钟超时窗口, 处理 4G 模块通信.
 *
 * @param from_standby true=从 light sleep 来, false=从 deep sleep 来
 */
void power_manager_gps_poll_cycle(bool from_standby);

#ifdef __cplusplus
}
#endif

#endif /* POWER_MANAGER_H */