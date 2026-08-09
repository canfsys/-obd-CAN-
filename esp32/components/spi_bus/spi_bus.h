/**
 * @file spi_bus.h
 * @brief SPI 总线驱动 — 共享 SPI2_HOST, 支持多设备独立配置
 *
 * 该模块管理 SPI2_HOST 总线的初始化, 并提供统一的寄存器读写接口.
 * 多个 SPI 设备 (SIT1145、MCP2515) 共享同一条总线, 通过 CS 片选切换,
 * 每个设备可独立配置时钟速度和 SPI 模式.
 */

#ifndef SPI_BUS_H
#define SPI_BUS_H

#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPI 总线默认配置 */
#define SPI_BUS_CLOCK_SPEED_HZ  (4 * 1000 * 1000)  /* 默认时钟 4MHz        */
#define SPI_BUS_HOST            SPI2_HOST           /* 使用 SPI2 控制器     */
#define SPI_BUS_DMA_CHAN        SPI_DMA_CH_AUTO     /* DMA 通道自动分配     */

/**
 * @brief 初始化 SPI2_HOST 总线
 *
 * 配置 MOSI/MISO/SCK 引脚, 启用 DMA, 设置最大传输单元 32 字节.
 *
 * @param mosi_io  MOSI 引脚号
 * @param miso_io  MISO 引脚号
 * @param sclk_io  SCK 引脚号
 * @return esp_err_t
 */
esp_err_t spi_bus_init_board(int mosi_io, int miso_io, int sclk_io);

/**
 * @brief 在 SPI 总线上添加一个设备
 *
 * 每个设备拥有独立的 CS 引脚、时钟速度和 SPI 模式.
 * ESP-IDF 驱动在片选切换时自动切换时钟频率.
 *
 * @param cs_io        CS 片选引脚
 * @param clock_speed  时钟频率 (Hz), 如 1000000 = 1MHz
 * @param mode         SPI 模式 (0~3)
 * @param out_handle   输出设备句柄
 * @return esp_err_t
 */
esp_err_t spi_bus_add_device_custom(int cs_io, int clock_speed, int mode,
                                    spi_device_handle_t *out_handle);

/**
 * @brief 删除 SPI 设备 (释放资源, 用于 light sleep 唤醒后重建)
 *
 * 在重新添加设备前调用, 释放旧的 SPI 设备句柄占用的 DMA/队列资源.
 * 内部调用 ESP-IDF 的 spi_bus_remove_device().
 *
 * @param handle  SPI 设备句柄
 * @return esp_err_t
 */
esp_err_t spi_bus_del_device(spi_device_handle_t handle);

/**
 * @brief 为 SPI 设备设置名称 (用于日志打印)
 *
 * 注册 handle 到名称的映射, 后续 SPI 收发日志中会显示该名称.
 * 例如: "SIT1145-1 TX(16bit): 52 03 | RX: A0"
 *
 * @param handle  SPI 设备句柄
 * @param name    设备名称 (如 "SIT1145-1", "MCP2515")
 */
void spi_bus_set_device_name(spi_device_handle_t handle, const char *name);

/**
 * @brief 写 SPI 寄存器 (16-bit 事务: 地址 + 数据)
 *
 * @param handle  设备句柄
 * @param addr    寄存器地址 (8-bit)
 * @param data    要写入的数据
 * @return esp_err_t
 */
esp_err_t spi_bus_write_reg(spi_device_handle_t handle, uint8_t addr, uint8_t data);

/**
 * @brief 读 SPI 寄存器 (16-bit 事务: 地址 + 数据)
 *
 * @param handle   设备句柄
 * @param addr     寄存器地址 (8-bit)
 * @param out_data 输出读回的数据
 * @return esp_err_t
 */
esp_err_t spi_bus_read_reg(spi_device_handle_t handle, uint8_t addr, uint8_t *out_data);

/**
 * @brief 原始 SPI 全双工传输
 *
 * 发送 tx_data 同时接收 rx_data, 长度为 len bits.
 *
 * @param handle  设备句柄
 * @param tx_data 发送数据 (可为 NULL)
 * @param rx_data 接收缓冲 (可为 NULL)
 * @param len     传输位长度
 * @return esp_err_t
 */
esp_err_t spi_bus_transfer(spi_device_handle_t handle,
                           const uint8_t *tx_data, uint8_t *rx_data, int len);

#ifdef __cplusplus
}
#endif

#endif /* SPI_BUS_H */