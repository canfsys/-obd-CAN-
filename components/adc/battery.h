/**
 * @file battery.h
 * @brief 电池电压检测驱动 — ADC 采样
 *
 * 使用 ESP32-S3 的 ADC (oneshot 模式) 采集电池分压后的电压,
 * 提供电压读取 (mV) 和百分比估算功能.
 *
 * 硬件设计: 电池电压经 2:1 分压后送入 ADC 引脚,
 *           读取值 × 2 = 实际电池电压.
 */

#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电池电压检测
 *
 * 通过 GPIO 号自动获取 ADC 单元和通道号,
 * 配置 12-bit 精度, 12dB 衰减 (可测 0~3.3V).
 *
 * @param adc_pin       ADC 采样引脚 (GPIO11)
 * @param meas_en_pin   测量使能引脚 (GPIO39), -1 则不用
 * @return esp_err_t
 */
esp_err_t battery_adc_init(int adc_pin, int meas_en_pin);

/**
 * @brief 读取电池电压 (mV)
 *
 * 多采样取平均, 开启测量使能后延时 10ms 等待稳定.
 *
 * @param out_mv  输出电压 (毫伏)
 * @return esp_err_t
 */
esp_err_t battery_read_mv(uint32_t *out_mv);

/**
 * @brief 读取电池电压并估算百分比
 *
 * 基于锂电池放电曲线简化线性映射:
 *   4200mV → 100%, 3300mV → 0%
 *
 * @param out_percent  输出百分比 (0~100)
 * @return esp_err_t
 */
esp_err_t battery_read_percent(uint8_t *out_percent);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_H */