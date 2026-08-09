/**
 * @file power_manager.c
 * @brief 电源管理模式管理器实现
 *
 * 正常模式 → 1 分钟无 CAN → light sleep (3min GPS)
 * light sleep 3min → 定时器唤醒 → [由 sensor_task 执行 GPS 轮询]
 * light sleep 3次 → deep sleep (3min GPS)
 * light/deep sleep → GPIO1 唤醒 → 正常模式
 */

#include "power_manager.h"
#include "board_config.h"
#include "spi_bus.h"
#include "sit1145.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "POWER_MGR";
static power_mode_t s_mode = POWER_MODE_NORMAL;
static uint64_t s_last_can_ms = 0;
static bool s_should_standby = false;
static bool s_periph_initialized = false;  /* SPI/CAN 外设是否已初始化 */

static spi_device_handle_t s_h_sit1 = NULL;
static spi_device_handle_t s_h_sit2 = NULL;
static spi_device_handle_t s_h_mcp  = NULL;
static sit1145_dev_t s_sit1_dev = SIT1145_DEV_1;
static sit1145_dev_t s_sit2_dev = SIT1145_DEV_2;
static TaskHandle_t s_sd_task_handle = NULL;

extern void power_manager_wake_reinit(void);

/* --- GPIO 控制 --- */
static void power_gpio_standby_enter(void)
{
    gpio_set_level(PIN_SD_PWR_EN, 0);
    gpio_set_level(PIN_BAT_MEAS_EN, 0);
}

static void power_gpio_standby_exit(void)
{
    gpio_set_direction(PIN_SD_PWR_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_SD_PWR_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(PIN_BAT_MEAS_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BAT_MEAS_EN, 1);
}

static void power_gpio_sleep_enter(void)
{
    gpio_set_level(PIN_PWR_3V8_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int all_pins[] = {2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,38,39,40,41,42,47};
    for (size_t i = 0; i < sizeof(all_pins)/sizeof(all_pins[0]); i++)
        gpio_set_direction(all_pins[i], GPIO_MODE_DISABLE);
}

/* --- SPI/SD 注册 --- */
void power_manager_register_spi_handles(spi_device_handle_t h_sit1, spi_device_handle_t h_sit2, spi_device_handle_t h_mcp)
{
    s_h_sit1 = h_sit1; s_h_sit2 = h_sit2; s_h_mcp = h_mcp;
    s_periph_initialized = true;
}

void power_manager_register_sd_task(TaskHandle_t task_handle)
{
    s_sd_task_handle = task_handle;
}

/* --- API --- */
void power_manager_init(void)
{
    power_manager_set_mode(POWER_MODE_NORMAL);
    power_manager_mark_can_activity();
    s_periph_initialized = true;
    ESP_LOGI(TAG, "电源管理器初始化完成, NORMAL");
}

void power_manager_mark_can_activity(void)
{
    s_last_can_ms = esp_timer_get_time() / 1000;
    s_should_standby = false;
}

power_mode_t power_manager_get_mode(void) { return s_mode; }
void power_manager_set_mode(power_mode_t mode) { s_mode = mode; }

uint64_t power_manager_get_idle_ms(void)
{
    uint64_t now = esp_timer_get_time() / 1000;
    return (now > s_last_can_ms) ? (now - s_last_can_ms) : 0;
}

esp_sleep_wakeup_cause_t power_manager_get_wakeup_cause(void) { return esp_sleep_get_wakeup_cause(); }

bool power_manager_is_auto_wake(void)
{
    return (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
}

bool power_manager_should_standby(void) { return s_should_standby; }

void power_manager_tick(uint64_t now_ms)
{
    if (s_mode != POWER_MODE_NORMAL) return;
    uint64_t idle_ms = (now_ms > s_last_can_ms) ? (now_ms - s_last_can_ms) : 0;
    if (idle_ms >= STANDBY_IDLE_TIMEOUT_MS) {
        s_should_standby = true;
        ESP_LOGI(TAG, "CAN 空闲 %llu ms, 待机", idle_ms);
    }
}

/* --- SIT1145 休眠 --- */
static esp_err_t sit1145_sleep_sequence(sit1145_dev_t dev)
{
    static const uint8_t seq[][2] = {
        {0x4C,0x03},{0x20,0x01},{0x04,0x00},{0x23,0x01},
        {0x61,0xFF},{0x63,0xFF},{0x64,0xFF},{0x01,0x01},
    };
    const char *names[] = {"0x4C","0x20","0x04","0x23","0x61","0x63","0x64","0x01"};
    for (size_t i = 0; i < 8; i++) {
        esp_err_t ret = sit1145_write_reg(dev, seq[i][0], seq[i][1]);
        if (ret != ESP_OK) { ESP_LOGW(TAG, "SIT1145-%d 写 %s 失败", dev+1, names[i]); return ret; }
        ESP_LOGI(TAG, "SIT1145-%d 写 %s = 0x%02X", dev+1, names[i], seq[i][1]);
        esp_rom_delay_us(10);
        if (seq[i][0] == 0x61) {
            uint8_t rd=0; if (sit1145_read_reg(dev,0x61,&rd)==ESP_OK) ESP_LOGI(TAG,"  回读 0x61=0x%02X",rd);
        }
        if (seq[i][0] == 0x64) {
            uint8_t rd=0; if (sit1145_read_reg(dev,0x64,&rd)==ESP_OK) ESP_LOGI(TAG,"  回读 0x64=0x%02X",rd);
        }
    }
    return ESP_OK;
}

/* --- MCP2515 休眠 --- */
static esp_err_t mcp2515_sleep_sequence(spi_device_handle_t handle)
{
    uint8_t tx[4];
    tx[0]=0x05; tx[1]=0x2C; tx[2]=0x40; tx[3]=0x00;
    ESP_LOGI(TAG, "MCP2515: 清除 WAKIF"); spi_bus_transfer(handle,tx,NULL,32);
    tx[0]=0x05; tx[1]=0x2B; tx[2]=0x40; tx[3]=0x40;
    ESP_LOGI(TAG, "MCP2515: 使能 WAKIE"); spi_bus_transfer(handle,tx,NULL,32);
    tx[0]=0x05; tx[1]=0x0F; tx[2]=0xE0; tx[3]=0x20;
    ESP_LOGI(TAG, "MCP2515: 休眠"); spi_bus_transfer(handle,tx,NULL,32);
    return ESP_OK;
}

/* --- 进入 light sleep --- */
void power_manager_enter_standby(void)
{
    ESP_LOGI(TAG, "===== light sleep =====");
    s_mode = POWER_MODE_STANDBY;

    /* 仅在外设已初始化时才执行 SPI/CAN 休眠 (GPS 轮询后不执行) */
    if (s_periph_initialized) {
        if (s_h_sit1) sit1145_sleep_sequence(s_sit1_dev);
        if (s_h_sit2) sit1145_sleep_sequence(s_sit2_dev);
        if (s_h_mcp) mcp2515_sleep_sequence(s_h_mcp);
        s_periph_initialized = false;
        ESP_LOGI(TAG, "SPI/CAN 休眠完成 (已清除初始化标志)");
    } else {
        ESP_LOGI(TAG, "跳过 SPI/CAN 休眠 (GPS 轮询阶段)");
    }

    power_gpio_standby_enter();

    printf("rtc_gpio_is_valid_gpio(GPIO1) = %d\n", rtc_gpio_is_valid_gpio(GPIO_NUM_1));
    if (rtc_gpio_is_valid_gpio(GPIO_NUM_1)) {
        rtc_gpio_init(GPIO_NUM_1);
        rtc_gpio_set_direction(GPIO_NUM_1, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(GPIO_NUM_1);
        rtc_gpio_pulldown_en(GPIO_NUM_1);
    }

    /* GPIO42 (非 RTC IO): 配置 IO MUX 低电平唤醒 (仅 light sleep)
     * UART RX 空闲高电平不唤醒, 收到数据时起始位(低电平)唤醒.
     * 注意: 需要 gpio_sleep_sel_dis() + 与 EXT1 分开配置 */
    gpio_set_direction(GPIO_NUM_42, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_42, GPIO_PULLDOWN_ONLY);
    gpio_sleep_sel_dis(GPIO_NUM_42);
    gpio_wakeup_enable(GPIO_NUM_42, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    esp_sleep_enable_timer_wakeup(GPS_POLL_INTERVAL_US);
    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_1, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    ESP_LOGI(TAG, "进入 light sleep (3min GPS, GPIO1/42 唤醒)");
    fflush(stdout);
    esp_light_sleep_start();

    /* --- 唤醒后 --- */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const char *cs = "未知";
    switch(cause) {
        case ESP_SLEEP_WAKEUP_EXT1: cs="EXT1(GPIO1)"; break;
        case ESP_SLEEP_WAKEUP_TIMER: cs="RTC定时器"; break;
        case ESP_SLEEP_WAKEUP_GPIO: cs="IO MUX GPIO"; break;
        case ESP_SLEEP_WAKEUP_UART: cs="UART"; break;
        case ESP_SLEEP_WAKEUP_WIFI: cs="WiFi"; break;
        case ESP_SLEEP_WAKEUP_BT: cs="蓝牙"; break;
        case ESP_SLEEP_WAKEUP_UNDEFINED: cs="未定义/冲突"; break;
        default: cs="其他"; break;
    }
    ESP_LOGI(TAG, "唤醒 原因=%d(%s)", cause, cs);

    rtc_gpio_deinit(GPIO_NUM_1);
    power_gpio_standby_exit();

    return;
}

/* --- 进入 deep sleep --- */
void power_manager_enter_sleep(void)
{
    ESP_LOGI(TAG, "===== deep sleep =====");
    s_mode = POWER_MODE_SLEEP;

    power_gpio_sleep_enter();

    if (rtc_gpio_is_valid_gpio(GPIO_NUM_1)) {
        rtc_gpio_init(GPIO_NUM_1);
        rtc_gpio_set_direction(GPIO_NUM_1, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(GPIO_NUM_1);
        rtc_gpio_pulldown_en(GPIO_NUM_1);
    }
    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_1, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    ESP_LOGI(TAG, "进入 deep sleep, 仅 GPIO1");
    fflush(stdout);
    esp_deep_sleep_start();
}