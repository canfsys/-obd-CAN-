/**
 * @file sd_card.h
 * @brief SD 卡驱动 — SDMMC 4-bit 模式, 支持 CD 热插拔
 *
 * 使用 ESP32-S3 的 SDMMC 外设以 4-bit 模式访问 SD 卡,
 * 支持 GPIO CD (Card Detect) 引脚, 实现热插拔检测.
 * 挂载点: /sdcard
 */

#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SD 卡挂载点路径 */
#define SD_MOUNT_POINT  "/sdcard"

/**
 * @brief 初始化 SD 卡 (SDMMC 4-bit 模式, 带 CD 检测)
 *
 * 先复位相关 GPIO, 控制电源使能, 配置 CD 引脚,
 * 然后以 4-bit SDMMC 模式挂载 FAT 文件系统.
 * 如果 CD 引脚电平指示无卡, 则返回 ESP_ERR_NOT_FOUND.
 *
 * @param pwr_en_pin  电源使能引脚 (如 PIN_SD_PWR_EN), -1 则不控制
 * @param cd_pin      CD 卡检测引脚 (低电平=有卡), -1 则不检测
 * @return esp_err_t  ESP_OK=成功, ESP_ERR_NOT_FOUND=无卡, 其他=失败
 */
esp_err_t sd_card_init(int pwr_en_pin, int cd_pin);

/**
 * @brief 卸载 SD 卡
 */
void sd_card_deinit(void);

/**
 * @brief 检查 SD 卡是否已挂载
 *
 * @return true=已挂载, false=未挂载
 */
bool sd_card_is_mounted(void);

/**
 * @brief 检查 SD 卡是否插入 (读取 CD 引脚电平)
 *
 * @param cd_pin  CD 引脚号, -1 则返回 true (无 CD 检测)
 * @return true=有卡插入, false=无卡
 */
bool sd_card_is_present(int cd_pin);

/**
 * @brief 检查 CD 状态是否发生变化 (由 sensor_task 轮询)
 *
 * 当 CD 引脚产生 GPIO 中断时, 内部标志置位.
 * 调用此函数会清除标志.
 *
 * @return true=CD 状态变化 (需检查 sd_card_get_cd_level)
 */
bool sd_card_cd_changed(void);

/**
 * @brief 获取当前 CD 引脚电平
 *
 * @return 0=有卡插入, 1=无卡
 */
int sd_card_get_cd_level(void);

/**
 * @brief 获取 SD 卡总容量 (字节)
 *
 * @return uint64_t 总字节数
 */
uint64_t sd_card_get_total_bytes(void);

/**
 * @brief 获取 SD 卡剩余容量 (字节)
 *
 * @return uint64_t 剩余字节数 (粗略估算)
 */
uint64_t sd_card_get_free_bytes(void);

/**
 * @brief 写文件到 SD 卡
 *
 * 以二进制写入模式打开文件, 写入指定长度的数据后关闭.
 *
 * @param path    文件路径 (如 "/sdcard/test.txt")
 * @param data    要写入的数据
 * @param len     数据长度
 * @return esp_err_t
 */
esp_err_t sd_card_write_file(const char *path, const uint8_t *data, size_t len);

/**
 * @brief 从 SD 卡读取文件
 *
 * 以二进制读取模式打开文件, 读到缓冲区后关闭.
 *
 * @param path      文件路径
 * @param buffer    输出缓冲区
 * @param buf_size  缓冲区大小
 * @param out_len   输出实际读取的字节数
 * @return esp_err_t
 */
esp_err_t sd_card_read_file(const char *path, uint8_t *buffer, size_t buf_size, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_H */
