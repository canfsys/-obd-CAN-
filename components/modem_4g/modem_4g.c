/**
 * @file modem_4g.c
 * @brief 4G 模块驱动实现 — UART AT 命令通信
 *
 * 使用 ESP-IDF 的 UART 驱动 (UART_NUM_1),
 * 实现 4G 模块的初始化、复位、AT 命令发送和原始数据收发.
 */

#include "modem_4g.h"
#include "board_config.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "MODEM_4G";

/* UART 配置参数 */
#define MODEM_UART_NUM      UART_NUM_1      /* 使用 UART1              */
#define MODEM_BUF_SIZE      (1024)           /* 发送缓冲区大小          */
#define MODEM_RX_BUF_SIZE   (1024)           /* 接收缓冲区大小          */

static int s_reset_pin = -1;                 /* 模块复位引脚号          */

esp_err_t modem_4g_init(int tx_pin, int rx_pin, int reset_pin, uint32_t baudrate)
{
    s_reset_pin = reset_pin;

    /* 配置复位引脚, 默认保持高电平 (不复位) */
    if (reset_pin >= 0) {
        gpio_set_direction(reset_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(reset_pin, 1);
    }

    /* 配置 UART 参数 */
    uart_config_t uart_cfg = {
        .baud_rate  = (int)baudrate,         /* 波特率                  */
        .data_bits  = UART_DATA_8_BITS,      /* 8 位数据                */
        .parity     = UART_PARITY_DISABLE,   /* 无校验                  */
        .stop_bits  = UART_STOP_BITS_1,      /* 1 位停止位              */
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE, /* 无硬件流控          */
    };

    esp_err_t ret = uart_param_config(MODEM_UART_NUM, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART 参数配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 设置 TX/RX 引脚 */
    ret = uart_set_pin(MODEM_UART_NUM, tx_pin, rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART 引脚设置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 安装 UART 驱动 */
    ret = uart_driver_install(MODEM_UART_NUM, MODEM_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART 驱动安装失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "4G 模块初始化完成 (TX=%d, RX=%d, RST=%d, %lu baud)",
             tx_pin, rx_pin, reset_pin, (unsigned long)baudrate);
    return ESP_OK;
}

esp_err_t modem_4g_reset(void)
{
    if (s_reset_pin < 0) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "复位 4G 模块...");
    gpio_set_level(s_reset_pin, 0);           /* 拉低复位引脚          */
    vTaskDelay(pdMS_TO_TICKS(100));            /* 保持 100ms            */
    gpio_set_level(s_reset_pin, 1);           /* 释放复位              */
    vTaskDelay(pdMS_TO_TICKS(2000));           /* 等待模块启动 (2s)     */

    ESP_LOGI(TAG, "4G 模块复位完成");
    return ESP_OK;
}

esp_err_t modem_4g_send_at(const char *cmd, char *response, size_t resp_size, uint32_t timeout_ms)
{
    /* 清空接收缓冲, 避免收到上次残留数据 */
    uart_flush(MODEM_UART_NUM);

    /* 发送 AT 命令 */
    int len = strlen(cmd);
    uart_write_bytes(MODEM_UART_NUM, cmd, len);
    ESP_LOGD(TAG, "AT >> %s", cmd);

    /* 等待并读取响应 */
    int rx_len = uart_read_bytes(MODEM_UART_NUM, (uint8_t *)response,
                                  resp_size - 1, pdMS_TO_TICKS(timeout_ms));
    if (rx_len > 0) {
        response[rx_len] = '\0';               /* 字符串终止符          */
        ESP_LOGD(TAG, "AT << %s", response);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "AT 命令超时: %s", cmd);
    return ESP_ERR_TIMEOUT;
}

esp_err_t modem_4g_send_data(const uint8_t *data, size_t len)
{
    int written = uart_write_bytes(MODEM_UART_NUM, data, len);
    if (written != len) {
        ESP_LOGE(TAG, "发送不完整: %d/%zu", written, len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t modem_4g_receive_data(uint8_t *buffer, size_t buf_size, size_t *out_len)
{
    int len = uart_read_bytes(MODEM_UART_NUM, buffer, buf_size, 0); /* 非阻塞 */
    if (len < 0) return ESP_FAIL;
    if (out_len) *out_len = (size_t)len;
    return (len > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}