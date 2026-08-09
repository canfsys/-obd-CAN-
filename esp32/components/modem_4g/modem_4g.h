/**
 * @file modem_4g.h
 * @brief 4G 模块驱动 — UART AT 命令通信
 *
 * 通过 UART1 与 4G 模块通信, 支持 AT 命令发送/接收,
 * 模块复位控制, 以及原始数据收发.
 */

#ifndef MODEM_4G_H
#define MODEM_4G_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 4G 模块
 *
 * 配置复位引脚, 初始化 UART1 (波特率可配),
 * 为后续 AT 命令通信做准备.
 *
 * @param tx_pin      UART TX 引脚 (ESP→模块)
 * @param rx_pin      UART RX 引脚 (模块→ESP)
 * @param reset_pin   复位引脚 (低电平复位), -1 则不使用
 * @param baudrate    波特率 (如 115200)
 * @return esp_err_t
 */
esp_err_t modem_4g_init(int tx_pin, int rx_pin, int reset_pin, uint32_t baudrate);

/**
 * @brief 复位 4G 模块 (拉低复位引脚 100ms)
 *
 * @return esp_err_t
 */
esp_err_t modem_4g_reset(void);

/**
 * @brief 发送 AT 命令并等待响应 (阻塞)
 *
 * 先清空 UART 接收缓冲, 发送 AT 命令字符串,
 * 在超时时间内等待模块响应.
 *
 * @param cmd       AT 命令字符串 (如 "AT\r\n")
 * @param response  输出响应缓冲区
 * @param resp_size 缓冲区大小
 * @param timeout_ms 超时时间 (毫秒)
 * @return esp_err_t  ESP_OK=收到响应, ESP_ERR_TIMEOUT=超时
 */
esp_err_t modem_4g_send_at(const char *cmd, char *response, size_t resp_size, uint32_t timeout_ms);

/**
 * @brief 发送原始数据 (非阻塞)
 *
 * @param data  数据指针
 * @param len   数据长度
 * @return esp_err_t
 */
esp_err_t modem_4g_send_data(const uint8_t *data, size_t len);

/**
 * @brief 接收原始数据 (非阻塞)
 *
 * @param buffer     输出缓冲区
 * @param buf_size   缓冲区大小
 * @param out_len    输出实际接收长度
 * @return esp_err_t  ESP_OK=收到数据, ESP_ERR_NOT_FOUND=无数据
 */
esp_err_t modem_4g_receive_data(uint8_t *buffer, size_t buf_size, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MODEM_4G_H */