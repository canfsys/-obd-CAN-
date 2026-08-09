/**
 * @file mcp2515.h
 * @brief MCP2515 SPI 接口 CAN 控制器驱动
 *
 * MCP2515 是 Microchip 的独立 CAN 控制器, 通过 SPI 接口与 MCU 通信.
 * 本驱动提供初始化、CAN 帧收发、GPIO 中断通知功能.
 *
 * INT 引脚下降沿触发中断 → ISR 通过 FreeRTOS Task Notification 通知任务.
 */

#ifndef MCP2515_H
#define MCP2515_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"   /* for TaskHandle_t */
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MCP2515 标准 CAN 帧结构
 */
typedef struct {
    uint32_t id;            /*!< CAN ID: 标准帧 11-bit, 扩展帧 29-bit */
    uint8_t  dlc;           /*!< 数据长度码 (0~8)                      */
    uint8_t  data[8];       /*!< 数据 (最多 8 字节)                     */
    bool     is_extended;   /*!< true=扩展帧, false=标准帧              */
    bool     is_rtr;        /*!< true=远程帧                           */
} mcp2515_frame_t;

/**
 * @brief 初始化 MCP2515 CAN 控制器
 *
 * 初始化序列:
 *   1. SPI 复位 (0xC0)
 *   2. 配置波特率 (CNF1/CNF2/CNF3)
 *   3. 关闭过滤器 (RXB0CTRL/RXB1CTRL = 0x60)
 *   4. 关闭 RXnBF 引脚中断 (BFPCTRL = 0x00)
 *   5. 开启 INT 引脚中断 (CANINTE = 0x03)
 *   6. 清除所有中断标志 (CANINTF = 0x00)
 *   7. 配置为正常模式 (CANCTRL = 0x00)
 *
 * 同时安装 GPIO 中断服务, INT 引脚下降沿触发 ISR,
 * ISR 通过 xTaskNotifyGive 通知已注册的任务.
 *
 * @param handle    SPI 设备句柄
 * @param int_pin   中断引脚 (GPIO8, 低电平有效)
 * @return esp_err_t
 */
esp_err_t mcp2515_init(spi_device_handle_t handle, int int_pin);

/**
 * @brief 注册接收通知任务
 *
 * 当 MCP2515 INT 引脚产生下降沿中断时,
 * ISR 会调用 xTaskNotifyGive(notify_task) 通知该任务.
 *
 * @param notify_task  要通知的任务句柄
 */
void mcp2515_register_task(TaskHandle_t notify_task);

/**
 * @brief 发送 CAN 帧 (通过 TXB0 缓冲器)
 *
 * @param frame  指向要发送的帧
 * @return esp_err_t
 */
esp_err_t mcp2515_send_frame(const mcp2515_frame_t *frame);

/**
 * @brief 接收 CAN 帧 (非阻塞)
 *
 * 接收流程:
 *   1. 读 RX 状态 (0xB0) 判断哪个缓冲器满
 *   2. 读取 14 字节数据 (0x90/0x94 命令 + 13字节)
 *   3. 位修改清除中断标志
 *
 * @param frame    输出接收到的帧
 * @return esp_err_t  ESP_OK=收到帧, ESP_ERR_NOT_FOUND=无数据
 */
esp_err_t mcp2515_receive_frame(mcp2515_frame_t *frame);

/**
 * @brief 检查是否有中断 (INT 引脚电平)
 *
 * @return true=有中断等待处理
 */
bool mcp2515_has_interrupt(void);

#ifdef __cplusplus
}
#endif

#endif /* MCP2515_H */