/**
 * @file main.c
 * @brief 系统主入口 — ESP32-S3 车载终端
 *
 * 唤醒类型判断:
 *   - 上电/GPIO 唤醒 → 全初始化
 *   - 定时器自主唤醒 → GPS 轮询
 *
 * GPS 轮询逻辑 (进入 sleep 前设置 3 分钟定时器):
 *   light sleep 阶段 (前 10 分钟, 约 3 次):
 *     → GPS 轮询 2 分钟 → 回到 light sleep (GPIO21=高)
 *   deep sleep 阶段 (10 分钟后):
 *     → GPIO21=高 → GPS 轮询 2 分钟 → GPIO21=低 → deep sleep
 *   GPIO1 外部唤醒 → 全初始化
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/twai.h"
#include "board_config.h"
#include "spi_bus.h"
#include "sit1145.h"
#include "mcp2515.h"
#include "twai_driver.h"
#include "sd_card.h"
#include "sd_logger.h"
#include "can_buffer.h"
#include "can_parser.h"
#include "modem_4g.h"
#include "ds18b20.h"
#include "battery.h"
#include "power_manager.h"
#include "gps_poller.h"

static const char *TAG = "MAIN";

/* RTC 内存变量 (跨 deep sleep 保留) */
RTC_DATA_ATTR static uint8_t s_auto_wake_flag = 0;  /* 1=自主唤醒标志 */
RTC_DATA_ATTR static uint8_t s_wake_count = 0;      /* light sleep 定时器唤醒计数 (>=3 进 deep) */

/* PSRAM 双缓冲区 */
static can_buffer_ctx_t s_mcp_buf;
static can_buffer_ctx_t s_twai_buf;
static SemaphoreHandle_t s_sd_sem;

/* 任务句柄 */
static TaskHandle_t s_mcp2515_task_handle = NULL;
static TaskHandle_t s_twai_task_handle = NULL;
static TaskHandle_t s_sd_write_task_handle = NULL;

/* SPI 设备句柄 */
static spi_device_handle_t h_sit1, h_sit2, h_mcp;

/* 前向声明 */
static void power_manager_enter_auto_sleep(void);
static void wake_reinit(void);

/* ====================================================================== */
/*                          外设初始化                                     */
/* ====================================================================== */

static void init_all(void)
{
    ESP_LOGI(TAG, "===== 全初始化 =====");

    strcpy(g_state.deviceId, "car001");
    strcpy(g_state.carState, "online");

    gpio_set_direction(PIN_PWR_3V8_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_PWR_3V8_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_ERROR_CHECK(spi_bus_init_board(PIN_SPI_MOSI, PIN_SPI_MISO, PIN_SPI_SCK));
    ESP_ERROR_CHECK(spi_bus_add_device_custom(PIN_SIT1_CS, 1 * 1000 * 1000, 1, &h_sit1));
    spi_bus_set_device_name(h_sit1, "SIT1145-1");
    ESP_ERROR_CHECK(spi_bus_add_device_custom(PIN_SIT2_CS, 1 * 1000 * 1000, 1, &h_sit2));
    spi_bus_set_device_name(h_sit2, "SIT1145-2");
    ESP_ERROR_CHECK(spi_bus_add_device_custom(PIN_MCP2515_CS, 8 * 1000 * 1000, 0, &h_mcp));
    spi_bus_set_device_name(h_mcp, "MCP2515");

    g_state.sit1145_1_init = (sit1145_init(SIT1145_DEV_1, h_sit1) == ESP_OK) ? 1 : 0;
    g_state.sit1145_2_init = (sit1145_init(SIT1145_DEV_2, h_sit2) == ESP_OK) ? 1 : 0;
    sit1145_verify_chip_id(SIT1145_DEV_1);
    sit1145_verify_chip_id(SIT1145_DEV_2);
    g_state.mcp2515_init = (mcp2515_init(h_mcp, PIN_MCP2515_INT) == ESP_OK) ? 1 : 0;

    esp_err_t twai_ret = twai_driver_init(PIN_TWAI_TX, PIN_TWAI_RX);
    if (twai_ret == ESP_OK) twai_driver_start();
    else ESP_LOGW(TAG, "TWAI 跳过");

    esp_err_t sd_ret = sd_card_init(PIN_SD_PWR_EN, PIN_SD_CD);
    if (sd_ret == ESP_OK) {
        sd_logger_init("sit1145");
        sd_logger_init("mcp2515");
    } else ESP_LOGW(TAG, "SD 卡失败");

    size_t psram_size = esp_psram_get_size();
    if (psram_size > 0) ESP_LOGI(TAG, "PSRAM: %zu bytes", psram_size);
    else ESP_LOGW(TAG, "PSRAM 不可用");

    can_buffer_init(&s_mcp_buf, "mcp2515");
    can_buffer_init(&s_twai_buf, "sit1145");
    s_sd_sem = xSemaphoreCreateCounting(10, 0);
    can_buffer_set_shared_sem(&s_mcp_buf, s_sd_sem);
    can_buffer_set_shared_sem(&s_twai_buf, s_sd_sem);

    esp_err_t ret4g = modem_4g_init(PIN_4G_TX, PIN_4G_RX, PIN_4G_RESET, 115200);
    if (ret4g == ESP_OK) {
        char resp[128] = {0};
        modem_4g_send_at("AT\r\n", resp, sizeof(resp), 1000);
        ESP_LOGI(TAG, "4G AT: %s", resp);
    }

    ds18b20_init(PIN_DS18B20);
    if (ds18b20_is_present()) ESP_LOGI(TAG, "DS18B20 ✓");

    esp_err_t bat_ret = battery_adc_init(PIN_BAT_ADC, PIN_BAT_MEAS_EN);
    if (bat_ret != ESP_OK) ESP_LOGW(TAG, "ADC 跳过");

    ESP_LOGI(TAG, "===== 全初始化完成 =====");
}

/* ====================================================================== */
/*  外设重新初始化 (light sleep GPIO 唤醒后)                                */
/* ====================================================================== */

static void wake_reinit(void)
{
    ESP_LOGI(TAG, "===== [LIGHT SLEEP 唤醒] 重新初始化外设 =====");

    /* 步骤 1/5: 重新添加 SPI 设备 */
    ESP_LOGI(TAG, "[1/5] 重新添加 SPI 设备...");
    spi_bus_del_device(h_sit1);
    spi_bus_del_device(h_sit2);
    spi_bus_del_device(h_mcp);
    ESP_ERROR_CHECK(spi_bus_add_device_custom(PIN_SIT1_CS, 1 * 1000 * 1000, 1, &h_sit1));
    spi_bus_set_device_name(h_sit1, "SIT1145-1");
    ESP_ERROR_CHECK(spi_bus_add_device_custom(PIN_SIT2_CS, 1 * 1000 * 1000, 1, &h_sit2));
    spi_bus_set_device_name(h_sit2, "SIT1145-2");
    ESP_ERROR_CHECK(spi_bus_add_device_custom(PIN_MCP2515_CS, 8 * 1000 * 1000, 0, &h_mcp));
    spi_bus_set_device_name(h_mcp, "MCP2515");

    /* 步骤 2/5: 重新初始化 SIT1145 */
    ESP_LOGI(TAG, "[2/5] 初始化 SIT1145...");
    sit1145_init(SIT1145_DEV_1, h_sit1);
    sit1145_init(SIT1145_DEV_2, h_sit2);

    /* 步骤 3/5: 重新初始化 MCP2515 */
    ESP_LOGI(TAG, "[3/5] 初始化 MCP2515...");
    mcp2515_init(h_mcp, PIN_MCP2515_INT);

    /* 步骤 4/5: 重新挂载 SD 卡 */
    ESP_LOGI(TAG, "[4/5] 挂载 SD 卡...");
    esp_err_t sd_ret = sd_card_init(PIN_SD_PWR_EN, PIN_SD_CD);
    if (sd_ret == ESP_OK) {
        sd_logger_init("sit1145");
        sd_logger_init("mcp2515");
    }

    /* 重新启动 TWAI (进入 standby 前仅 stop, 仍 installed, 直接 start 即可) */
    twai_driver_start();

    /* 步骤 5/5: 注册句柄 */
    ESP_LOGI(TAG, "[5/5] 注册句柄...");
    power_manager_register_spi_handles(h_sit1, h_sit2, h_mcp);
    mcp2515_register_task(s_mcp2515_task_handle);

    ESP_LOGI(TAG, "===== 外设重新初始化完成 =====");
}

/* ====================================================================== */
/*  GPS 轮询 + 睡眠循环                                                    */
/* ====================================================================== */

static void power_manager_enter_auto_sleep(void)
{
    ESP_LOGI(TAG, "--- GPS 轮询完成, 准备再次进入 sleep ---");

    if (s_wake_count >= 3) {
        gpio_set_level(PIN_PWR_3V8_EN, 0);
        ESP_LOGI(TAG, "GPIO21=低, 进入 deep sleep");
        s_wake_count = 0;
        power_manager_set_mode(POWER_MODE_SLEEP);
        power_manager_enter_sleep();
    } else {
        ESP_LOGI(TAG, "GPIO21=高, 继续 light sleep (wake_count=%d)", s_wake_count);
        power_manager_set_mode(POWER_MODE_STANDBY);
        power_manager_enter_standby();
    }
}

/* ====================================================================== */
/*  任务                                                                   */
/* ====================================================================== */

static void mcp2515_task(void *arg)
{
    s_mcp2515_task_handle = xTaskGetCurrentTaskHandle();
    mcp2515_register_task(s_mcp2515_task_handle);
    ESP_LOGI(TAG, "[Core0] MCP2515 任务启动");

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (power_manager_get_mode() != POWER_MODE_NORMAL) continue;
        while (mcp2515_has_interrupt() && power_manager_get_mode() == POWER_MODE_NORMAL) {
            mcp2515_frame_t frame;
            if (mcp2515_receive_frame(&frame) == ESP_OK) {
                can_buffer_write_csv(&s_mcp_buf, frame.id, frame.dlc,
                                     frame.data, frame.is_extended, frame.is_rtr);
                can_parser_process(frame.id, frame.data, frame.dlc);
                power_manager_mark_can_activity();
            } else break;
        }
    }
}

static void twai_task(void *arg)
{
    s_twai_task_handle = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "[Core1] TWAI 任务启动");
    twai_frame_t frame;

    while (1) {
        bool has_data = twai_driver_wait_rx(1000);
        if (power_manager_get_mode() != POWER_MODE_NORMAL) continue;
        if (has_data) {
            while (twai_driver_receive(&frame, 0) == ESP_OK) {
                can_buffer_write_csv(&s_twai_buf, frame.id, frame.dlc,
                                     frame.data, frame.is_extended, frame.is_rtr);
                can_parser_process(frame.id, frame.data, frame.dlc);
                power_manager_mark_can_activity();
            }
        }
    }
}

static void sd_write_task(void *arg)
{
    s_sd_write_task_handle = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "SD 写入任务启动");

    while (1) {
        xSemaphoreTake(s_sd_sem, portMAX_DELAY);
        char *data = NULL; uint32_t size = 0;
        if (can_buffer_get_full(&s_mcp_buf, &data, &size)) {
            sd_logger_write("mcp2515", data, size);
            can_buffer_release_full(&s_mcp_buf);
        }
        if (can_buffer_get_full(&s_twai_buf, &data, &size)) {
            sd_logger_write("sit1145", data, size);
            can_buffer_release_full(&s_twai_buf);
        }
    }
}

static void sensor_task(void *arg)
{
    int counter = 0;
    ESP_LOGI(TAG, "传感器任务启动");

    while (1) {
        counter++;
        vTaskDelay(pdMS_TO_TICKS(1000));

        power_manager_tick(esp_timer_get_time() / 1000);

        if (power_manager_should_standby()) {
            ESP_LOGI(TAG, "===== 空闲超时, 进入待机 =====");

        vTaskSuspend(s_mcp2515_task_handle);
            vTaskSuspend(s_twai_task_handle);
            vTaskSuspend(s_sd_write_task_handle);
            /* TWAI 驱动保持 installed 状态 (仅停止通信), 避免 light sleep 唤醒后硬件残留中断 */
            twai_driver_stop();

            /* 卸载 4G 模块的 UART1 驱动 (GPS 轮询会重新安装) */
            uart_driver_delete(UART_NUM_1);

            char *data = NULL; uint32_t size = 0;
            if (can_buffer_get_full(&s_mcp_buf, &data, &size)) {
                sd_logger_write("mcp2515", data, size);
                can_buffer_release_full(&s_mcp_buf);
            }
            if (can_buffer_get_full(&s_twai_buf, &data, &size)) {
                sd_logger_write("sit1145", data, size);
                can_buffer_release_full(&s_twai_buf);
            }
            sd_card_deinit();

            /* 清零计数, 进入 light sleep 阶段 */
            s_auto_wake_flag = 1;
            s_wake_count = 0;

            /* GPIO21 保持高电平, 进入 light sleep */
            gpio_set_direction(PIN_PWR_3V8_EN, GPIO_MODE_OUTPUT);
            gpio_set_level(PIN_PWR_3V8_EN, 1);
            power_manager_set_mode(POWER_MODE_STANDBY);
            power_manager_enter_standby();

            /* 从 light sleep 唤醒 */
            if (power_manager_is_auto_wake()) {
                /* 定时器唤醒 → GPS 轮询 */
                ESP_LOGI(TAG, "定时器唤醒 (wake_count=%d/3)", s_wake_count);

                gps_poller_uart_init();
                gps_poller_run_cycle();
                gps_poller_uart_deinit();

                s_wake_count++;
                s_auto_wake_flag = 1;

                if (s_wake_count >= 3) {
                    /* 达到 3 次 → 进 deep sleep, GPIO21=低 */
                    gpio_set_level(PIN_PWR_3V8_EN, 0);
                    ESP_LOGI(TAG, "wake_count=%d, 进入 deep sleep", s_wake_count);
                    s_wake_count = 0;
                    power_manager_set_mode(POWER_MODE_SLEEP);
                    power_manager_enter_sleep();
                } else {
                    /* 继续 light sleep, GPIO21=高 */
                    ESP_LOGI(TAG, "wake_count=%d, 继续 light sleep", s_wake_count);
                    power_manager_set_mode(POWER_MODE_STANDBY);
                    power_manager_enter_standby();
                }
                /* power_manager_enter_sleep/standby 不会返回(会再次 sleep),
                   但如果从 deep sleep 回来则重启, 从 light sleep 回来则回到这里 */
                /* 重新进来后再次判断唤醒类型 */
            } else {
                /* GPIO 唤醒 → 全初始化 */
                s_auto_wake_flag = 0;
                wake_reinit();
                vTaskResume(s_mcp2515_task_handle);
                vTaskResume(s_twai_task_handle);
                vTaskResume(s_sd_write_task_handle);
                power_manager_set_mode(POWER_MODE_NORMAL);
                power_manager_mark_can_activity();
                ESP_LOGI(TAG, "===== 外部唤醒, 恢复正常 =====");
            }
        }

        /* SD 卡热插拔检测 (CD 中断触发后检查) */
        if (sd_card_cd_changed()) {
            if (sd_card_get_cd_level() == 0) {
                ESP_LOGI(TAG, "SD 卡插入, 尝试挂载...");
                if (sd_card_init(PIN_SD_PWR_EN, PIN_SD_CD) == ESP_OK) {
                    sd_logger_init("sit1145");
                    sd_logger_init("mcp2515");
                    ESP_LOGI(TAG, "SD 卡热插挂载成功");
                }
            } else {
                ESP_LOGI(TAG, "SD 卡拔出, 卸载...");
                sd_card_deinit();
            }
        }

        if (counter % 10 == 0) {
            int32_t temp = 0;
            if (ds18b20_read_temp(&temp) == ESP_OK)
                g_state.temperature = (float)temp / 1000.0f;
            uint32_t bat_mv = 0; uint8_t bat_pct = 0;
            if (battery_read_mv(&bat_mv) == ESP_OK) {
                battery_read_percent(&bat_pct);
                g_state.voltage = (float)bat_mv / 1000.0f;
                g_state.fuel = bat_pct;
            }
            ESP_LOGI(TAG, "[STATE] %s V=%.2f fuel=%d temp=%.1f idle=%llu",
                     g_state.carState, g_state.voltage, g_state.fuel,
                     g_state.temperature,
                     (unsigned long long)power_manager_get_idle_ms());
        }
    }
}

/* ====================================================================== */
/*  app_main                                                               */
/* ====================================================================== */

void app_main(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause == ESP_SLEEP_WAKEUP_TIMER && s_auto_wake_flag) {
        /* ===== 定时器自主唤醒: GPS 轮询 ===== */
        ESP_LOGI(TAG, "自主唤醒 (wake_count=%d/3)", s_wake_count);

        gps_poller_uart_init();
        gps_poller_run_cycle();
        gps_poller_uart_deinit();

        s_wake_count++;
        s_auto_wake_flag = 1;

        if (s_wake_count >= 3) {
            gpio_set_level(PIN_PWR_3V8_EN, 0);
            ESP_LOGI(TAG, "wake_count=%d, 进入 deep sleep", s_wake_count);
            s_wake_count = 0;
            power_manager_set_mode(POWER_MODE_SLEEP);
            power_manager_enter_sleep();
        } else {
            ESP_LOGI(TAG, "wake_count=%d, 继续 light sleep", s_wake_count);
            power_manager_set_mode(POWER_MODE_STANDBY);
            power_manager_enter_standby();
        }
        return;
    }

    /* ===== 上电 或 GPIO 唤醒 (deep sleep 恢复也走此路径) ===== */
    s_auto_wake_flag = 0;
    s_wake_count = 0;
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "[DEEP SLEEP 唤醒] GPIO1 外部唤醒, 全初始化");
    } else {
        ESP_LOGI(TAG, "上电复位: 全初始化");
    }

    init_all();
    power_manager_init();
    power_manager_register_spi_handles(h_sit1, h_sit2, h_mcp);

    UBaseType_t prio_highest = configMAX_PRIORITIES - 1;
    xTaskCreatePinnedToCore(mcp2515_task, "mcp2515", 3072, NULL, prio_highest, NULL, 0);
    xTaskCreatePinnedToCore(twai_task,    "twai",    3072, NULL, prio_highest, NULL, 1);
    xTaskCreatePinnedToCore(sd_write_task,"sd_write",4096, NULL, 1,            NULL, 1);
    xTaskCreatePinnedToCore(sensor_task,  "sensor",  4096, NULL, 5,            NULL, 1);

    vTaskDelete(NULL);
}