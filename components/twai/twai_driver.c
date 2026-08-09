/**
 * @file twai_driver.c
 * @brief TWAI (ESP32 内置 CAN) 驱动实现
 *
 * 封装 ESP-IDF 的 TWAI 驱动, 配置为 125kbps,
 * 提供安装/启动/停止/收发/Alert等待接口.
 *
 * Alert 中断:
 *   twai_read_alerts() 阻塞等待 RXI/DOI 硬件中断,
 *   无需轮询, 任务挂起至事件发生.
 */

#include "twai_driver.h"
#include "esp_log.h"
#include "driver/twai.h"
#include <string.h>

static const char *TAG = "TWAI";
static bool s_installed = false;        /* 驱动是否已安装标志 */
static bool s_alerts_configured = false; /* Alert 是否已配置    */

esp_err_t twai_driver_init(int tx_pin, int rx_pin)
{
    ESP_LOGI(TAG, "初始化 TWAI: TX=GPIO%d, RX=GPIO%d", tx_pin, rx_pin);

    /* 通用配置: 设置 TX/RX 引脚, 普通模式, 接收队列 128 帧 */
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)tx_pin, (gpio_num_t)rx_pin, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = 128;

    /* 时序配置: 使用 ESP-IDF 内置的 125kbps 宏 */
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();

    /* 滤波配置: 接收所有帧 */
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret == ESP_OK) {
        s_installed = true;
        ESP_LOGI(TAG, "TWAI 驱动安装成功 (125kbps)");
    } else {
        ESP_LOGE(TAG, "TWAI 驱动安装失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t twai_driver_start(void)
{
    if (!s_installed) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = twai_start();
    if (ret == ESP_OK) {
        /* 配置 Alert: 启用 RX 数据到达和 RX 队列满中断 */
        ret = twai_reconfigure_alerts(
            TWAI_ALERT_RX_DATA | TWAI_ALERT_RX_QUEUE_FULL, NULL);
        if (ret == ESP_OK) {
            s_alerts_configured = true;
            ESP_LOGI(TAG, "TWAI 通信启动, Alert 已配置 (RXI + DOI)");
        } else {
            ESP_LOGW(TAG, "Alert 配置失败: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "TWAI 启动失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t twai_driver_stop(void)
{
    if (!s_installed) return ESP_ERR_INVALID_STATE;
    s_alerts_configured = false;
    ESP_LOGI(TAG, "停止 TWAI 通信");
    return twai_stop();
}

void twai_driver_deinit(void)
{
    if (!s_installed) return;
    ESP_LOGI(TAG, "卸载 TWAI 驱动");
    twai_stop();
    twai_driver_uninstall();
    s_installed = false;
    s_alerts_configured = false;
}

esp_err_t twai_driver_transmit(const twai_frame_t *frame, uint32_t timeout_ms)
{
    /* 将自定义帧结构转换为 ESP-IDF 的 twai_message_t */
    twai_message_t msg = {
        .identifier = frame->id,
        .data_length_code = frame->dlc,
        .extd = frame->is_extended,
        .rtr = frame->is_rtr,
    };
    /* 复制数据 (排除远程帧, 远程帧无数据) */
    if (frame->dlc > 0 && frame->dlc <= 8 && !frame->is_rtr) {
        memcpy(msg.data, frame->data, frame->dlc);
    }

    esp_err_t ret = twai_transmit(&msg, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "发送失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t twai_driver_receive(twai_frame_t *frame, uint32_t timeout_ms)
{
    twai_message_t msg;
    esp_err_t ret = twai_receive(&msg, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) return ret;  /* 超时或无数据 */

    /* 将 ESP-IDF 消息转换为自定义帧结构 */
    frame->id = msg.identifier;
    frame->dlc = msg.data_length_code;
    frame->is_extended = msg.extd;
    frame->is_rtr = msg.rtr;

    /* 复制数据 */
    if (frame->dlc > 0 && frame->dlc <= 8 && !frame->is_rtr) {
        memcpy(frame->data, msg.data, frame->dlc);
    }

    /* 打印 TWAI 接收到的帧数据 */
    ESP_LOGI(TAG, "TWAI RX: ID=0x%03lX DLC=%d Data=%02X %02X %02X %02X %02X %02X %02X %02X%s%s",
             (unsigned long)frame->id, frame->dlc,
             frame->dlc >= 1 ? frame->data[0] : 0,
             frame->dlc >= 2 ? frame->data[1] : 0,
             frame->dlc >= 3 ? frame->data[2] : 0,
             frame->dlc >= 4 ? frame->data[3] : 0,
             frame->dlc >= 5 ? frame->data[4] : 0,
             frame->dlc >= 6 ? frame->data[5] : 0,
             frame->dlc >= 7 ? frame->data[6] : 0,
             frame->dlc >= 8 ? frame->data[7] : 0,
             frame->is_extended ? " EXT" : "",
             frame->is_rtr ? " RTR" : "");

    return ESP_OK;
}

bool twai_driver_wait_rx(uint32_t timeout_ms)
{
    if (!s_alerts_configured) return false;

    uint32_t alerts;
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ?
                        portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    esp_err_t ret = twai_read_alerts(&alerts, ticks);
    if (ret != ESP_OK) return false;

    if (alerts & TWAI_ALERT_RX_DATA) {
        return true;  /* RX FIFO 中有数据待读取 */
    }

    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
        ESP_LOGW(TAG, "TWAI RX FIFO 溢出, 立即读取!");
        return true;  /* 溢出也需要读取 */
    }

    return false;
}