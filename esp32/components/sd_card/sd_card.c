/**
 * @file sd_card.c
 * @brief SD 卡驱动实现 — SDMMC 4-bit 模式, 支持 CD 热插拔
 *
 * 使用 ESP-IDF 的 sdmmc 驱动和 FAT 文件系统,
 * 实现 SD 卡的初始化挂载、CD 检测、热插拔等功能.
 */

#include "sd_card.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "SD_CARD";
static sdmmc_card_t *s_card = NULL;     /* SDMMC 卡对象指针 */
static bool s_mounted = false;           /* 挂载标志         */
static int s_cd_pin = -1;                /* CD 引脚号        */
static gpio_num_t s_cd_gpio = GPIO_NUM_NC;

/* CD 引脚中断标志 (由 ISR 设置, sensor_task 轮询检查) */
static volatile bool s_cd_changed = false;
static volatile int s_cd_level = 1;      /* 当前 CD 电平 (1=无卡, 0=有卡) */
static bool s_isr_service_installed = false;  /* ISR 服务是否已安装 */

/* ---------- CD 引脚中断 ISR ---------- */

static void IRAM_ATTR sd_cd_isr_handler(void *arg)
{
    int level = gpio_get_level((gpio_num_t)(intptr_t)arg);
    s_cd_level = level;
    s_cd_changed = true;
}

/* ---------- 公开 API ---------- */

bool sd_card_is_present(int cd_pin)
{
    if (cd_pin < 0) return true;
    return gpio_get_level((gpio_num_t)cd_pin) == 0;
}

esp_err_t sd_card_init(int pwr_en_pin, int cd_pin)
{
    /* 保存 CD 引脚配置 */
    s_cd_pin = cd_pin;
    s_cd_gpio = (cd_pin >= 0) ? (gpio_num_t)cd_pin : GPIO_NUM_NC;

    /* 配置 CD 引脚 GPIO 和中断 (无论是否有卡都要注册, 以便插卡时检测) */
    if (cd_pin >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << cd_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&io_conf);

        /* GPIO ISR 服务由 mcp2515_init 在前面的初始化中已安装, 直接注册中断处理 */
        gpio_isr_handler_add((gpio_num_t)cd_pin, sd_cd_isr_handler, (void *)(intptr_t)cd_pin);
        ESP_LOGI(TAG, "CD 中断已注册 (GPIO%d, ANYEDGE)", cd_pin);

        /* 检测 CD 引脚电平 */
        if (!sd_card_is_present(cd_pin)) {
            ESP_LOGW(TAG, "CD 检测: 无 SD 卡插入, 跳过挂载 (等待热插拔)");
            s_cd_level = 1;
            return ESP_ERR_NOT_FOUND;
        }
        s_cd_level = 0;
        ESP_LOGI(TAG, "CD 检测: SD 卡已插入 (GPIO%d=低)", cd_pin);
    }

    /* 复位 SD 卡相关 GPIO (清除上电初始状态) */
    gpio_reset_pin(PIN_SD_CLK);
    gpio_reset_pin(PIN_SD_CMD);
    gpio_reset_pin(PIN_SD_D0);
    gpio_reset_pin(PIN_SD_D1);
    gpio_reset_pin(PIN_SD_D2);
    gpio_reset_pin(PIN_SD_D3);

    /* 可选: SD 卡电源使能控制 */
    if (pwr_en_pin >= 0) {
        gpio_set_direction(pwr_en_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pwr_en_pin, 1);  /* 高电平开启供电 */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    vTaskDelay(pdMS_TO_TICKS(100));  /* 等待 SD 卡上电稳定 */

    /* 配置 SDMMC 主机 (4-bit 模式) */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;  /* 使用 SDMMC 插槽 1 */

    /* 引脚映射配置 */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0  = PIN_SD_D0;
    slot_config.d1  = PIN_SD_D1;
    slot_config.d2  = PIN_SD_D2;
    slot_config.d3  = PIN_SD_D3;
    slot_config.width = 4;                             /* 4-bit 模式 */
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* CD 引脚不传递给 slot_config (ESP-IDF v5.3 SDMMC 驱动对 CD 支持有限),
     * 初始化前已手动检查 CD 电平, 热插拔由 CD 中断 + sensor_task 处理 */
    /* FAT 文件系统挂载配置 */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,               /* 挂载失败不格式化 */
        .max_files = 5,                                 /* 最大同时打开文件数 */
        .allocation_unit_size = 16 * 1024,              /* 分配单元 16KB */
    };

    /* 挂载 FAT 文件系统 */
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡挂载失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);  /* 打印 SD 卡信息 */
    ESP_LOGI(TAG, "SD 卡初始化成功 (CD 中断已在前面注册)");

    return ESP_OK;
}

void sd_card_deinit(void)
{
    if (s_mounted) {
        /* 注意: 不移除 CD 中断处理, 保持监听引脚状态以便热插拔检测 */
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_mounted = false;
        s_card = NULL;
        ESP_LOGI(TAG, "SD 卡已卸载 (CD 中断保持活跃)");
    }
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

/**
 * @brief 检查 CD 状态是否发生变化 (由 sensor_task 调用)
 *
 * @return true=CD 状态变化, false=无变化
 */
bool sd_card_cd_changed(void)
{
    if (!s_cd_changed) return false;
    s_cd_changed = false;
    return true;
}

/**
 * @brief 获取当前 CD 引脚电平
 *
 * @return 0=有卡, 1=无卡
 */
int sd_card_get_cd_level(void)
{
    return s_cd_level;
}

uint64_t sd_card_get_total_bytes(void)
{
    if (!s_card) return 0;
    return (uint64_t)s_card->csd.capacity;  /* 从 CSD 寄存器读取 */
}

uint64_t sd_card_get_free_bytes(void)
{
    /* 简化处理: 返回总容量的 80% 作为粗略剩余空间 */
    return sd_card_get_total_bytes() * 80 / 100;
}

esp_err_t sd_card_write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "打开文件写入失败: %s, errno=%d", path, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "写入不完整: %zu/%zu", written, len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "写入文件成功: %s (%zu bytes)", path, len);
    return ESP_OK;
}

esp_err_t sd_card_read_file(const char *path, uint8_t *buffer, size_t buf_size, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开文件读取失败: %s, errno=%d", path, errno);
        return ESP_FAIL;
    }

    size_t read = fread(buffer, 1, buf_size, f);
    fclose(f);

    if (out_len) *out_len = read;
    ESP_LOGI(TAG, "读取文件成功: %s (%zu bytes)", path, read);
    return ESP_OK;
}