/**
 * @file battery.c
 * @brief 电池电压检测驱动实现 — ADC 采样
 *
 * 使用 ESP-IDF 的 adc_oneshot 驱动, 自动通过 GPIO 号获取 ADC 通道.
 * 采样 8 次取平均, 测量时控制使能引脚以降低功耗.
 * 电压换算: ADC_raw × 3300 / 4095 × 2 (2:1 分压).
 */

#include "battery.h"
#include "board_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BATTERY";
static int s_meas_en_pin = -1;                       /* 测量使能引脚      */
static adc_oneshot_unit_handle_t s_adc_handle = NULL; /* ADC 单元句柄     */
static adc_channel_t s_adc_channel = ADC_CHANNEL_0;   /* ADC 通道号       */

esp_err_t battery_adc_init(int adc_pin, int meas_en_pin)
{
    s_meas_en_pin = meas_en_pin;

    /* 通过 GPIO 号自动获取 ADC 单元和通道号 */
    adc_unit_t adc_unit;
    esp_err_t ret = adc_oneshot_io_to_channel(adc_pin, &adc_unit, &s_adc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d 不是有效的 ADC 引脚", adc_pin);
        return ret;
    }
    ESP_LOGI(TAG, "GPIO%d → ADC%d 通道%d", adc_pin, adc_unit + 1, s_adc_channel);

    /* 创建 ADC 单元 */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = adc_unit,
    };
    ret = adc_oneshot_new_unit(&init_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC%d 单元创建失败: %s", adc_unit + 1, esp_err_to_name(ret));
        return ret;
    }

    /* 配置 ADC 通道: 12-bit 精度, 12dB 衰减 (可测 0~3.3V) */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 测量使能引脚初始化为输出, 低电平 (关闭分压, 省电) */
    if (meas_en_pin >= 0) {
        gpio_set_direction(meas_en_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(meas_en_pin, 0);
    }

    ESP_LOGI(TAG, "电池 ADC 初始化完成 (GPIO=%d, EN=%d)", adc_pin, meas_en_pin);
    return ESP_OK;
}

esp_err_t battery_read_mv(uint32_t *out_mv)
{
    if (!out_mv || !s_adc_handle) return ESP_ERR_INVALID_ARG;

    /* 开启测量使能 (开启分压电路) */
    if (s_meas_en_pin >= 0) {
        gpio_set_level(s_meas_en_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(10));  /* 等待电压稳定 */
    }

    /* 采样 8 次取平均, 降低噪声 */
    int raw = 0;
    int sum = 0;
    const int samples = 8;
    for (int i = 0; i < samples; i++) {
        if (adc_oneshot_read(s_adc_handle, s_adc_channel, &raw) == ESP_OK) {
            sum += raw;
        }
        vTaskDelay(pdMS_TO_TICKS(2));  /* 采样间隔 2ms */
    }

    /* 关闭测量使能 (关闭分压, 省电) */
    if (s_meas_en_pin >= 0) {
        gpio_set_level(s_meas_en_pin, 0);
    }

    if (sum == 0) {
        ESP_LOGW(TAG, "ADC 采样全为 0");
        *out_mv = 0;
        return ESP_OK;
    }

    /* 计算平均 AD 值 */
    int avg_raw = sum / samples;

    /* 换算电压:
     *   ADC 参考电压 3.3V, 12-bit 精度 (0~4095)
     *   引脚电压 = avg_raw × 3300 / 4095 (mV)
     *   电池电压 = 引脚电压 × 2 (2:1 分压) */
    uint32_t voltage_mv = (uint32_t)avg_raw * 3300 / 4095;
    uint32_t battery_mv = voltage_mv * 2;

    *out_mv = battery_mv;
    ESP_LOGI(TAG, "ADC raw=%d, %lu mV -> 电池 %lu mV",
             avg_raw, (unsigned long)voltage_mv, (unsigned long)battery_mv);
    return ESP_OK;
}

esp_err_t battery_read_percent(uint8_t *out_percent)
{
    if (!out_percent) return ESP_ERR_INVALID_ARG;

    uint32_t mv;
    esp_err_t ret = battery_read_mv(&mv);
    if (ret != ESP_OK) return ret;

    /* 锂电池简化线性映射:
     *   4.20V → 100%
     *   3.30V → 0%
     *   (mV - 3300) * 100 / (4200 - 3300) */
    if (mv >= 4200) *out_percent = 100;
    else if (mv <= 3300) *out_percent = 0;
    else *out_percent = (uint8_t)((mv - 3300) * 100 / 900);

    return ESP_OK;
}