/**
 * @file spi_bus.c
 * @brief SPI 总线驱动实现 — 共享 SPI2_HOST
 *
 * 使用 ESP-IDF spi_master 驱动, 提供总线初始化、设备添加、寄存器读写等接口.
 * 支持 handle→name 映射, 在 SPI 日志中显示设备名.
 */

#include "spi_bus.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SPI_BUS";

/* ---------- SPI 设备名映射 (用于日志) ---------- */
#define SPI_NAME_MAP_MAX  8

typedef struct {
    spi_device_handle_t handle;
    char                name[24];
} spi_name_entry_t;

static spi_name_entry_t s_name_map[SPI_NAME_MAP_MAX] = {0};
static int s_name_map_count = 0;

static const char *spi_find_name(spi_device_handle_t handle)
{
    for (int i = 0; i < s_name_map_count; i++) {
        if (s_name_map[i].handle == handle) {
            return s_name_map[i].name;
        }
    }
    return "SPI_DEV";
}

/* ====================================================================== */
/*  公开 API                                                              */
/* ====================================================================== */

esp_err_t spi_bus_init_board(int mosi_io, int miso_io, int sclk_io)
{
    ESP_LOGI(TAG, "初始化 SPI2_HOST 总线: MOSI=%d, MISO=%d, SCK=%d",
             mosi_io, miso_io, sclk_io);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = mosi_io,
        .miso_io_num     = miso_io,
        .sclk_io_num     = sclk_io,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 32,
    };

    esp_err_t ret = spi_bus_initialize(SPI_BUS_HOST, &bus_cfg, SPI_BUS_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "总线初始化失败: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "总线初始化成功");
    }
    return ret;
}

esp_err_t spi_bus_add_device_custom(int cs_io, int clock_speed, int mode,
                                    spi_device_handle_t *out_handle)
{
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz   = clock_speed,
        .mode             = mode,
        .spics_io_num     = cs_io,
        .queue_size       = 1,
        .cs_ena_pretrans  = 1,
        .cs_ena_posttrans = 1,
    };

    esp_err_t ret = spi_bus_add_device(SPI_BUS_HOST, &dev_cfg, out_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加设备 (CS=%d) 失败: %s", cs_io, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "设备 (CS=%d) 添加成功, 速度=%dHz, mode=%d",
                 cs_io, clock_speed, mode);
    }
    return ret;
}

esp_err_t spi_bus_del_device(spi_device_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "删除 SPI 设备 (handle=0x%p)", (void *)handle);
    return spi_bus_remove_device(handle);
}

void spi_bus_set_device_name(spi_device_handle_t handle, const char *name)
{
    if (!handle || !name) return;

    /* 更新已存在的条目 */
    for (int i = 0; i < s_name_map_count; i++) {
        if (s_name_map[i].handle == handle) {
            strncpy(s_name_map[i].name, name, sizeof(s_name_map[i].name) - 1);
            s_name_map[i].name[sizeof(s_name_map[i].name) - 1] = '\0';
            ESP_LOGD(TAG, "设备 %s (handle=0x%p) 名称已更新", name, (void *)handle);
            return;
        }
    }

    /* 新增条目 */
    if (s_name_map_count < SPI_NAME_MAP_MAX) {
        s_name_map[s_name_map_count].handle = handle;
        strncpy(s_name_map[s_name_map_count].name, name,
                sizeof(s_name_map[s_name_map_count].name) - 1);
        s_name_map[s_name_map_count].name[sizeof(s_name_map[s_name_map_count].name) - 1] = '\0';
        s_name_map_count++;
        ESP_LOGD(TAG, "设备 %s (handle=0x%p) 已注册", name, (void *)handle);
    } else {
        ESP_LOGW(TAG, "SPI 设备名映射表已满, 无法注册 %s", name);
    }
}

esp_err_t spi_bus_write_reg(spi_device_handle_t handle, uint8_t addr, uint8_t data)
{
    uint8_t tx_buf[2] = {addr, data};
    esp_err_t ret = spi_bus_transfer(handle, tx_buf, NULL, 16);
    return ret;
}

esp_err_t spi_bus_read_reg(spi_device_handle_t handle, uint8_t addr, uint8_t *out_data)
{
    uint8_t tx_buf[2] = {addr, 0x00};
    uint8_t rx_buf[2] = {0, 0};
    esp_err_t ret = spi_bus_transfer(handle, tx_buf, rx_buf, 16);
    if (ret == ESP_OK && out_data) {
        *out_data = rx_buf[1];
    }
    return ret;
}

esp_err_t spi_bus_transfer(spi_device_handle_t handle,
                           const uint8_t *tx_data, uint8_t *rx_data, int len)
{
    spi_transaction_t t = {
        .length    = len,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };
    esp_err_t ret = spi_device_transmit(handle, &t);

    /* 打印 SPI 收发数据 (带设备名) */
    if (tx_data) {
        const char *dev_name = spi_find_name(handle);
        int byte_len = (len + 7) / 8;
        char hex_str[128] = {0};
        int pos = 0;
        for (int i = 0; i < byte_len && pos < (int)sizeof(hex_str) - 4; i++) {
            pos += snprintf(hex_str + pos, sizeof(hex_str) - pos,
                           "%02X ", tx_data[i]);
        }
        if (rx_data) {
            char rx_hex[128] = {0};
            pos = 0;
            for (int i = 0; i < byte_len && pos < (int)sizeof(rx_hex) - 4; i++) {
                pos += snprintf(rx_hex + pos, sizeof(rx_hex) - pos,
                               "%02X ", rx_data[i]);
            }
            ESP_LOGI(TAG, "%s TX(%dbit): %s| RX: %s", dev_name, len, hex_str, rx_hex);
        } else {
            ESP_LOGI(TAG, "%s TX(%dbit): %s| (无RX)", dev_name, len, hex_str);
        }
    }

    return ret;
}