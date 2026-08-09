/**
 * @file sd_logger.h
 * @brief SD 卡 CSV 日志写入器
 *
 * 将满缓冲区数据写入 SD 卡 CSV 文件.
 * 自动扫描文件夹中最大编号, 递增创建新文件.
 * 目录结构: /sdcard/sit1145/can_0001.csv
 *           /sdcard/mcp2515/can_0002.csv
 */

#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** CSV 文件最大路径长度 */
#define CSV_PATH_MAX   64

/**
 * @brief 初始化 SD 日志写入器
 *
 * 创建文件夹 (如 /sdcard/sit1145/) 并扫描文件名确定起始编号.
 *
 * @param subdir  子目录名 (如 "sit1145" 或 "mcp2515")
 * @return esp_err_t
 */
esp_err_t sd_logger_init(const char *subdir);

/**
 * @brief 将缓冲区数据写入新的 CSV 文件
 *
 * 创建递增编号的文件, 如 can_0003.csv,
 * 写入 CSV 头 + 缓冲区数据, 然后关闭文件.
 *
 * @param subdir   子目录名
 * @param data     缓冲区数据 (CSV 格式)
 * @param data_len 数据长度
 * @return esp_err_t
 */
esp_err_t sd_logger_write(const char *subdir, const char *data, uint32_t data_len);

#ifdef __cplusplus
}
#endif

#endif /* SD_LOGGER_H */