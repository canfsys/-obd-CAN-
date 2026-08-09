/**
 * @file ds18b20.h
 * @brief DS18B20 温度传感器驱动 (单总线协议)
 *
 * DS18B20 是 Maxim 的数字温度传感器, 使用单总线 (1-Wire) 协议通信.
 * 本驱动提供初始化、温度读取和存在检测功能.
 */

#ifndef DS18B20_H
#define DS18B20_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 DS18B20 引脚
 *
 * 配置 GPIO 为开漏输出 + 上拉, 准备单总线通信.
 *
 * @param gpio_pin  数据引脚号
 * @return esp_err_t
 */
esp_err_t ds18b20_init(int gpio_pin);

/**
 * @brief 读取温度
 *
 * 执行: 复位 → 跳过 ROM → 启动转换 → 等待 → 复位 → 跳过 ROM → 读暂存器
 *
 * @param out_temp_c  输出温度 (摄氏度 × 1000, 如 25.5°C = 25500)
 * @return esp_err_t  成功=ESP_OK, 检测不到=ESP_ERR_INVALID_RESPONSE
 */
esp_err_t ds18b20_read_temp(int32_t *out_temp_c);

/**
 * @brief 检测 DS18B20 是否存在
 *
 * 发送复位脉冲, 检查是否有器件发出存在脉冲.
 *
 * @return true=存在, false=不存在
 */
bool ds18b20_is_present(void);

#ifdef __cplusplus
}
#endif

#endif /* DS18B20_H */