/**
 * @file sit1145.h
 * @brief SIT1145AQ CAN 收发器 SPI 驱动
 *
 * SIT1145AQ 是一款 SPI 接口的 CAN 收发器, 支持 CAN FD.
 * 本驱动提供初始化、寄存器读写、睡眠模式控制和芯片 ID 验证功能.
 *
 * 地址编码: 7 位寄存器地址左移 1 位, LSB bit0 = R/W (0=写, 1=读)
 *   写地址: SIT1145_ADDR_W(addr) = (addr << 1) | 0
 *   读地址: SIT1145_ADDR_R(addr) = (addr << 1) | 1
 */

#ifndef SIT1145_H
#define SIT1145_H

#include <stdint.h>
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SIT1145AQ 芯片 ID 寄存器值 */
#define SIT1145_CHIP_ID      0x74

/* 地址编码宏: LSB = R/W bit */
#define SIT1145_ADDR_W(addr)  ((uint8_t)((addr) << 1 | 0))  /* 写地址 */
#define SIT1145_ADDR_R(addr)  ((uint8_t)((addr) << 1 | 1))  /* 读地址 */

/**
 * @brief SIT1145 设备实例 ID
 */
typedef enum {
    SIT1145_DEV_1 = 0,      /* CS=GPIO5, 连接 TWAI 路            */
    SIT1145_DEV_2,          /* CS=GPIO6, 连接 MCP2515 路          */
    SIT1145_DEV_MAX
} sit1145_dev_t;

/**
 * @brief 初始化 SIT1145 收发器
 *
 * 执行步骤:
 *   1. 写入初始化序列 (3 个寄存器)
 *   2. 写入 0x26=0x02, 0x20=0x32 进行寄存器读写验证
 *   3. 读回 0x22 检查是否为 0xA8, 若失败则重试最多 3 次
 *
 * @param dev    设备编号
 * @param handle SPI 设备句柄
 * @return esp_err_t
 */
esp_err_t sit1145_init(sit1145_dev_t dev, spi_device_handle_t handle);

/**
 * @brief 获取 SPI 设备句柄
 *
 * @param dev    设备编号
 * @param handle 输出句柄
 * @return esp_err_t
 */
esp_err_t sit1145_get_handle(sit1145_dev_t dev, spi_device_handle_t *handle);

/**
 * @brief 写 SIT1145 寄存器
 *
 * @param dev  设备编号
 * @param reg  寄存器地址 (7 位)
 * @param data 要写入的数据
 * @return esp_err_t
 */
esp_err_t sit1145_write_reg(sit1145_dev_t dev, uint8_t reg, uint8_t data);

/**
 * @brief 读 SIT1145 寄存器
 *
 * @param dev     设备编号
 * @param reg     寄存器地址 (7 位)
 * @param out_data 输出读回的数据
 * @return esp_err_t
 */
esp_err_t sit1145_read_reg(sit1145_dev_t dev, uint8_t reg, uint8_t *out_data);

/**
 * @brief 设置 SIT1145 进入休眠/待机模式
 *
 * 寄存器 0x81 bit0: 1=休眠, 0=待机
 *
 * @param dev         设备编号
 * @param sleep_mode  true=休眠, false=待机
 * @return esp_err_t
 */
esp_err_t sit1145_set_sleep(sit1145_dev_t dev, bool sleep_mode);

/**
 * @brief 读取 SIT1145 模式状态
 *
 * 读寄存器 0x01, 低 3 位 = MC 模式.
 * MC 值: 0=普通, 1=休眠, 2=待机, 3=进入待机, 4=准备进入CANFD
 *
 * @param dev     设备编号
 * @param out_mc  输出 MC 模式 (低 3 位)
 * @return esp_err_t
 */
esp_err_t sit1145_get_mode_status(sit1145_dev_t dev, uint8_t *out_mc);

/**
 * @brief 验证芯片 ID (读寄存器 0x7E, 期望 0x74)
 *
 * @param dev   设备编号
 * @return true=ID 正确, false=异常
 */
bool sit1145_verify_chip_id(sit1145_dev_t dev);

#ifdef __cplusplus
}
#endif

#endif /* SIT1145_H */