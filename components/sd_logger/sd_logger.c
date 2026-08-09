/**
 * @file sd_logger.c
 * @brief SD 卡 CSV 日志写入器实现
 *
 * 每次上电 sd_logger_init() 时扫描文件夹中最大编号, 递增创建新文件并写入 CSV 头.
 * 后续 sd_logger_write() 以追加模式写入该文件.
 * 目录结构: /sdcard/sit1145/can_0001.csv
 *           /sdcard/mcp2515/can_0002.csv
 */

#include "sd_logger.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>     /* for mkdir */
#include <errno.h>

static const char *TAG = "SD_LOG";

/** CSV 头行 (每个数据字节单独一列) */
#define CSV_HEADER "timestamp_ms,can_id,dlc,d0,d1,d2,d3,d4,d5,d6,d7,extended,rtr\r\n"

/* 每个子目录的文件路径 (0=sit1145, 1=mcp2515) */
static char s_file_path[2][CSV_PATH_MAX] = {{0}};

/**
 * @brief 获取子目录对应索引
 */
static int get_subdir_idx(const char *subdir)
{
    if (strcmp(subdir, "sit1145") == 0) return 0;
    if (strcmp(subdir, "mcp2515") == 0) return 1;
    return -1;
}

/**
 * @brief 通过试探文件是否存在, 找到下一个可用的 can_XXXX 编号
 *
 * 使用 fopen("rb") 检查文件是否存在, 比 opendir/readdir 更可靠.
 * @param subdir 子目录路径 (如 "/sdcard/sit1145")
 * @return 可用编号 (从 1 开始)
 */
static int find_next_file_number(const char *subdir)
{
    int num = 1;
    char test_path[CSV_PATH_MAX];
    while (1) {
        int len = snprintf(test_path, sizeof(test_path),
                           "/sdcard/%s/can_%04d.csv", subdir, num);
        if (len <= 0 || len >= CSV_PATH_MAX) return 1;

        FILE *f = fopen(test_path, "rb");
        if (!f) break;  /* 文件不存在, 使用此编号 */
        fclose(f);
        num++;
    }
    return num;
}

esp_err_t sd_logger_init(const char *subdir)
{
    if (!subdir) return ESP_ERR_INVALID_ARG;

    int idx = get_subdir_idx(subdir);
    if (idx < 0) return ESP_ERR_INVALID_ARG;

    /* 构造目录路径: /sdcard/subdir/ */
    char dir_path[CSV_PATH_MAX];
    int len = snprintf(dir_path, sizeof(dir_path), "/sdcard/%s", subdir);
    if (len <= 0 || len >= CSV_PATH_MAX) return ESP_ERR_INVALID_ARG;

    /* 创建文件夹 (如果不存在) */
    struct stat st;
    if (stat(dir_path, &st) != 0) {
        if (mkdir(dir_path, 0777) != 0) {
            if (errno != EEXIST) {
                ESP_LOGE(TAG, "创建目录 %s 失败: errno=%d", dir_path, errno);
                return ESP_FAIL;
            }
        }
    }

    /* 找到下一个可用编号 */
    int new_num = find_next_file_number(subdir);

    snprintf(s_file_path[idx], sizeof(s_file_path[idx]),
             "/sdcard/%s/can_%04d.csv", subdir, new_num);

    /* 创建新文件并写入 CSV 头 */
    FILE *f = fopen(s_file_path[idx], "wb");
    if (!f) {
        ESP_LOGE(TAG, "创建 %s 失败: errno=%d", s_file_path[idx], errno);
        return ESP_FAIL;
    }
    fwrite(CSV_HEADER, 1, strlen(CSV_HEADER), f);
    fclose(f);

    ESP_LOGI(TAG, "%s: 新文件 %s 已创建 (编号=%d)", subdir, s_file_path[idx], new_num);
    return ESP_OK;
}

esp_err_t sd_logger_write(const char *subdir, const char *data, uint32_t data_len)
{
    if (!subdir || !data || data_len == 0) return ESP_ERR_INVALID_ARG;

    int idx = get_subdir_idx(subdir);
    if (idx < 0) return ESP_ERR_INVALID_ARG;

    /* 检查文件路径是否已初始化 */
    if (s_file_path[idx][0] == '\0') {
        ESP_LOGE(TAG, "%s: 未调用 sd_logger_init", subdir);
        return ESP_ERR_INVALID_STATE;
    }

    /* 以追加模式打开文件 */
    FILE *f = fopen(s_file_path[idx], "ab");
    if (!f) {
        ESP_LOGE(TAG, "打开 %s 失败: errno=%d", s_file_path[idx], errno);
        return ESP_FAIL;
    }

    /* 追加写入缓冲区数据 */
    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);

    if (written != data_len) {
        ESP_LOGE(TAG, "%s: 写入不完整 %zu/%lu", subdir, written, (unsigned long)data_len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "%s: 追加写入 %s 完成 (+%lu bytes, +%lu frames)",
             subdir, s_file_path[idx], (unsigned long)data_len,
             (unsigned long)(data_len / 48));

    return ESP_OK;
}