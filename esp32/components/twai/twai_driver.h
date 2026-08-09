/**
 * @file twai_driver.h
 * @brief TWAI (ESP32 内置 CAN) 驱动封装
 *
 * 对 ESP-IDF 的 TWAI 驱动 (driver/twai.h) 进行封装,
 * 提供简化的初始化、收发接口, 以及 Alert 中断等待功能.
 *
 * Alert 机制: twai_read_alerts() 阻塞等待硬件中断,
 *   RXI (TWAI_ALERT_RX_DATA) → RX FIFO 有数据
 *   DOI (TWAI_ALERT_RX_QUEUE_FULL) → RX FIFO 溢出
 */

#ifndef TWAI_DRIVER_H
#define TWAI_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TWAI CAN 帧结构 (简化版)
 *
 * 包含 CAN 2.0 标准/扩展帧的所有必要字段.
 */
typedef struct {
    uint32_t id;            /*!< CAN ID: 标准帧 11-bit, 扩展帧 29-bit */
    uint8_t  dlc;           /*!< 数据长度码 (0~8)                      */
    uint8_t  data[8];       /*!< 数据 (最多 8 字节)                     */
    bool     is_extended;   /*!< true=扩展帧 (29-bit ID)                */
    bool     is_rtr;        /*!< true=远程帧 (Remote Transmit Request)  */
} twai_frame_t;

/**
 * @brief 初始化 TWAI 驱动
 *
 * 安装 TWAI 驱动, 使用 ESP-IDF 内置的 125kbps 时序宏.
 * 滤波器设置为接收所有帧.
 *
 * @param tx_pin  发送引脚 (连接 CAN 收发器的 TXD)
 * @param rx_pin  接收引脚 (连接 CAN 收发器的 RXD)
 * @return esp_err_t
 */
esp_err_t twai_driver_init(int tx_pin, int rx_pin);

/**
 * @brief 启动 TWAI 通信 (含 Alert 配置)
 *
 * 在 twai_driver_init 之后调用, 启用 RX_DATA 和 RX_QUEUE_FULL 警报.
 * 之后可使用 twai_driver_wait_rx() 阻塞等待数据.
 *
 * @return esp_err_t
 */
esp_err_t twai_driver_start(void);

/**
 * @brief 停止 TWAI 通信
 *
 * @return esp_err_t
 */
esp_err_t twai_driver_stop(void);

/**
 * @brief 卸载 TWAI 驱动 (完全释放资源)
 *
 * 先停止通信, 再卸载驱动. 用于 light sleep 进入前完全释放 TWAI 资源,
 * 以便唤醒后能重新调用 twai_driver_init().
 */
void twai_driver_deinit(void);

/**
 * @brief 发送 CAN 帧
 *
 * @param frame      指向要发送的帧
 * @param timeout_ms 超时时间 (毫秒)
 * @return esp_err_t
 */
esp_err_t twai_driver_transmit(const twai_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief 接收 CAN 帧 (阻塞)
 *
 * @param frame      输出接收到的帧
 * @param timeout_ms 超时时间 (毫秒)
 * @return esp_err_t  ESP_OK=收到帧, ESP_ERR_TIMEOUT=超时无数据
 */
esp_err_t twai_driver_receive(twai_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief 等待 TWAI RX 数据中断 (阻塞)
 *
 * 利用 TWAI Alert 机制阻塞等待 RX 数据.
 * 当 RX FIFO 中有待读取报文时立即返回.
 * 对应硬件 RXI 中断.
 *
 * @param timeout_ms 超时时间 (毫秒), portMAX_DELAY 表示无限等待
 * @return true=有数据待读, false=超时或错误
 */
bool twai_driver_wait_rx(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* TWAI_DRIVER_H */