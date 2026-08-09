/**
 * @file gps_poller.c
 * @brief 4G 模块 GPS 轮询器实现
 *
 * 在 sleep 唤醒后通过 UART1 与 4G 模块通信.
 * 模块使用私有协议 (非 AT 命令).
 * 通信格式: KEY=VALUE 多行文本.
 */

#include "gps_poller.h"
#include "board_config.h"
#include "can_parser.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "GPS_POLL";

/* UART1 配置 */
#define UART_PORT       UART_NUM_1
#define UART_BUF_SIZE   512
#define LINE_BUF_SIZE   128

/* 本地宏定义 (与 power_manager.h 中的值保持一致) */
#define MODEM_UART_BAUD       115200
#define GPS_POLL_WINDOW_MS    (2 * 60 * 1000)    /* 2 分钟 */
#define GPS_RETRY_INTERVAL_MS 10000               /* 10 秒 */

/* 接收缓冲 */
static char s_rx_buf[UART_BUF_SIZE];

/* ====================================================================== */
/*  UART 收发                                                               */
/* ====================================================================== */

void gps_poller_uart_init(void)
{
    ESP_LOGI(TAG, "===== GPS 轮询: UART1 初始化 =====");

    /* 拉高 4G 模块电源 */
    ESP_LOGI(TAG, "[1/5] GPIO21 = 高 (4G 模块上电)");
    gpio_set_direction(PIN_PWR_3V8_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_PWR_3V8_EN, 1);
    ESP_LOGI(TAG, "[2/5] 等待 100ms 模块稳定...");
    esp_rom_delay_us(100 * 1000);  /* 100ms 等待模块上电稳定 */

    /* 配置 UART1 引脚 */
    ESP_LOGI(TAG, "[3/5] 配置 UART1: TX=GPIO41, RX=GPIO42, 波特率=%d", MODEM_UART_BAUD);
    uart_config_t uart_cfg = {
        .baud_rate  = MODEM_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_PORT, &uart_cfg);
    uart_set_pin(UART_PORT, PIN_4G_TX, PIN_4G_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_PORT, UART_BUF_SIZE, 0, 0, NULL, 0);

    ESP_LOGI(TAG, "UART1 初始化完成 (TX=GPIO%d, RX=GPIO%d)",
             PIN_4G_TX, PIN_4G_RX);
}

void gps_poller_uart_deinit(void)
{
    ESP_LOGI(TAG, "===== GPS 轮询: 关闭 UART1 =====");
    ESP_LOGI(TAG, "[4/5] 卸载 UART1 驱动...");
    uart_driver_delete(UART_PORT);
    ESP_LOGI(TAG, "[5/5] GPIO21 = 低 (4G 模块断电)");
    gpio_set_level(PIN_PWR_3V8_EN, 0);
    ESP_LOGI(TAG, "===== UART1 已关闭, 4G 电源已断开 =====");
}

static void uart_flush_rx(void)
{
    int flushed = uart_flush_input(UART_PORT);
    ESP_LOGD(TAG, "清空 UART1 接收缓冲 (丢弃 %d 字节)", flushed);
}

static esp_err_t uart_send(const char *str)
{
    int len = strlen(str);
    ESP_LOGI(TAG, "UART1 发送: %s (%d bytes)", str, len);
    int written = uart_write_bytes(UART_PORT, str, len);
    if (written == len) {
        ESP_LOGI(TAG, "发送成功 ✓");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "发送失败: 期望 %d bytes, 实际 %d bytes", len, written);
        return ESP_FAIL;
    }
}

static esp_err_t uart_recv_all(char *buf, int buf_size, uint32_t timeout_ms)
{
    uint64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    int pos = 0;
    memset(buf, 0, buf_size);

    ESP_LOGI(TAG, "等待 UART1 接收 (超时 %lu ms)...", (unsigned long)timeout_ms);
    while (esp_timer_get_time() / 1000 < deadline && pos < buf_size - 1) {
        int len = uart_read_bytes(UART_PORT, (uint8_t *)&buf[pos],
                                  buf_size - 1 - pos, pdMS_TO_TICKS(200));
        if (len > 0) {
            pos += len;
            ESP_LOGD(TAG, "已接收 %d bytes", pos);
        } else {
            if (pos > 0) break;
        }
    }
    buf[pos] = '\0';
    ESP_LOGI(TAG, "UART1 接收完成: %d bytes, 超时=%s", pos,
             (pos > 0) ? "否(有数据)" : "是(无数据)");
    return (pos > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ====================================================================== */
/*  等待 "GPS=OK"                                                          */
/* ====================================================================== */

static bool wait_gps_ok(uint32_t timeout_ms)
{
    uint64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    char line[LINE_BUF_SIZE];
    int total_read = 0;

    ESP_LOGI(TAG, "等待模块上报 \"GPS=OK\" (%lu 秒超时)...",
             (unsigned long)(timeout_ms / 1000));

    while (esp_timer_get_time() / 1000 < deadline) {
        int len = uart_read_bytes(UART_PORT, (uint8_t *)line,
                                  sizeof(line) - 1, pdMS_TO_TICKS(500));
        if (len > 0) {
            line[len] = '\0';
            total_read += len;
            /* 去掉末尾换行再打印 */
            char *clean = line;
            while (*clean == '\r' || *clean == '\n') clean++;
            if (strlen(clean) > 0) {
                ESP_LOGI(TAG, "GPS_POLL RX: \"%s\"", clean);
            }
            if (strstr(line, "GPS=OK") != NULL) {
                ESP_LOGI(TAG, "✓ 收到 GPS=OK, 模块就绪 (共接收 %d bytes)", total_read);
                return true;
            }
        }
    }
    ESP_LOGW(TAG, "✗ 等待 GPS=OK 超时 (%lu ms, 共接收 %d bytes)",
             (unsigned long)timeout_ms, total_read);
    return false;
}

/* ====================================================================== */
/*  解析 KEY=VALUE 行                                                      */
/* ====================================================================== */

static void parse_line(const char *line)
{
    char key[32] = {0};
    char val[64] = {0};

    if (sscanf(line, "%31[^=]=%63[^\n]", key, val) != 2) {
        ESP_LOGD(TAG, "忽略非 KEY=VALUE 行: %s", line);
        return;
    }

    ESP_LOGI(TAG, "解析: %s = %s", key, val);

    if (strcmp(key, "NET") == 0) {
        g_state.net = (uint8_t)atoi(val);
        ESP_LOGI(TAG, "  → g_state.net = %d", g_state.net);
    } else if (strcmp(key, "CSQ") == 0) {
        g_state.csq = (uint8_t)atoi(val);
        ESP_LOGI(TAG, "  → g_state.csq = %d", g_state.csq);
    } else if (strcmp(key, "GPS") == 0) {
        g_state.gps = (uint8_t)atoi(val);
        ESP_LOGI(TAG, "  → g_state.gps = %d", g_state.gps);
    } else if (strcmp(key, "FIX") == 0) {
        g_state.fix = (uint8_t)atoi(val);
        ESP_LOGI(TAG, "  → g_state.fix = %d %s", g_state.fix,
                 g_state.fix ? "✓ 已定位" : "未定位");
    } else if (strcmp(key, "SAT") == 0) {
        g_state.sat = (uint8_t)atoi(val);
        ESP_LOGI(TAG, "  → g_state.sat = %d 颗卫星", g_state.sat);
    } else if (strcmp(key, "HDOP") == 0) {
        g_state.hdop = (float)atof(val);
        ESP_LOGI(TAG, "  → g_state.hdop = %.1f", g_state.hdop);
    } else if (strcmp(key, "LAT") == 0) {
        g_state.lat = atof(val);
        ESP_LOGI(TAG, "  → g_state.lat = %.6f", g_state.lat);
    } else if (strcmp(key, "LNG") == 0) {
        g_state.lng = atof(val);
        ESP_LOGI(TAG, "  → g_state.lng = %.6f", g_state.lng);
    } else if (strcmp(key, "ALT") == 0) {
        g_state.altitude = (float)atof(val);
        ESP_LOGI(TAG, "  → g_state.altitude = %.1f m", g_state.altitude);
    } else if (strcmp(key, "HEIGHT") == 0) {
        g_state.height = (float)atof(val);
        ESP_LOGI(TAG, "  → g_state.height = %.1f m", g_state.height);
    } else if (strcmp(key, "SPEED") == 0) {
        g_state.speed = (float)atof(val);
        ESP_LOGI(TAG, "  → g_state.speed = %.1f km/h", g_state.speed);
    } else if (strcmp(key, "TIME") == 0) {
        strncpy(g_state.gps_time, val, sizeof(g_state.gps_time) - 1);
        ESP_LOGI(TAG, "  → g_state.gps_time = %s", g_state.gps_time);
    } else {
        ESP_LOGD(TAG, "  → 未知 KEY, 跳过");
    }
}

/* ====================================================================== */
/*  发送 STATUS? 并解析响应                                                 */
/* ====================================================================== */

static bool send_status_and_parse(void)
{
    uart_flush_rx();

    /* 发送 STATUS? */
    if (uart_send("STATUS?\r\n") != ESP_OK) {
        return false;
    }

    /* 等待响应 (最长 2 秒) */
    if (uart_recv_all(s_rx_buf, sizeof(s_rx_buf), 2000) != ESP_OK) {
        ESP_LOGW(TAG, "STATUS? 无响应");
        return false;
    }

    /* 逐行解析缓冲区内容 */
    char *line = s_rx_buf;
    char *next;
    while (line && *line) {
        next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            /* 去掉行尾 \r */
            char *end = line + strlen(line) - 1;
            if (end >= line && *end == '\r') *end = '\0';
        }

        if (strlen(line) > 2) {
            ESP_LOGI(TAG, "RX: %s", line);
            parse_line(line);
        }

        line = next ? (next + 1) : NULL;
    }

    return true;
}

/* ====================================================================== */
/*  发送 JSON 格式的 system_state_t 数据                                    */
/* ====================================================================== */

static void send_state_json(void)
{
    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\n"
        "\"deviceId\": \"%s\",\n"
        "\"lat\": %.4f,\n"
        "\"lng\": %.4f,\n"
        "\"speed\": %.1f,\n"
        "\"hdop\": %.1f,\n"
        "\"altitude\": %.1f,\n"
        "\"height\": %.1f\n"
        "}\n",
        g_state.deviceId,
        g_state.lat, g_state.lng,
        g_state.speed,
        g_state.hdop,
        g_state.altitude,
        g_state.height);

    ESP_LOGI(TAG, "向 4G 模块发送 JSON 数据 (%d bytes):", len);
    ESP_LOGI(TAG, "--- JSON 开始 ---");
    ESP_LOGI(TAG, "%s", json);
    ESP_LOGI(TAG, "--- JSON 结束 ---");

    uart_write_bytes(UART_PORT, json, len);
    ESP_LOGI(TAG, "JSON 已通过 UART1 发送 ✓");
}

/* ====================================================================== */
/*  公开 API — 执行一次 GPS 轮询周期                                       */
/* ====================================================================== */

bool gps_poller_run_cycle(void)
{
    uint64_t start_ms = esp_timer_get_time() / 1000;
    int status_count = 0;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  GPS 轮询周期开始");
    ESP_LOGI(TAG, "  直接发送 STATUS?, 12 秒超时/次");
    ESP_LOGI(TAG, "============================================");

    /* 在 2 分钟窗口内反复发送 STATUS? */
    while (1) {
        uint64_t elapsed = esp_timer_get_time() / 1000 - start_ms;
        if (elapsed >= GPS_POLL_WINDOW_MS) {
            ESP_LOGI(TAG, "轮询窗口到期 (%llu 秒)", elapsed / 1000);
            break;
        }

        ESP_LOGI(TAG, "--- 第 %d 次 STATUS? (已用 %llu 秒) ---",
                 status_count + 1, elapsed / 1000);

        /* 直接发送 STATUS? 等待 12 秒 */
        uart_flush_rx();
        uart_send("STATUS?\r\n");

        char resp[512] = {0};
        uint64_t deadline = esp_timer_get_time() / 1000 + 12000;
        int pos = 0;

        while (esp_timer_get_time() / 1000 < deadline && pos < (int)sizeof(resp) - 1) {
            int len = uart_read_bytes(UART_PORT, (uint8_t *)&resp[pos],
                                      sizeof(resp) - 1 - pos, pdMS_TO_TICKS(2000));
            if (len > 0) {
                pos += len;
                /* 收到数据后延长时间等待更多 */
                deadline = esp_timer_get_time() / 1000 + 2000;
            } else {
                if (pos > 0) break;  /* 2秒无新数据 → 结束 */
            }
        }
        resp[pos] = '\0';

        if (pos > 0) {
            status_count++;
            /* 逐行解析 */
            char *line = resp;
            char *next;
            while (line && *line) {
                next = strchr(line, '\n');
                if (next) { *next = '\0'; char *e = line + strlen(line) - 1; if (e >= line && *e == '\r') *e = '\0'; }
                if (strlen(line) > 2) { ESP_LOGI(TAG, "RX: %s", line); parse_line(line); }
                line = next ? (next + 1) : NULL;
            }

            if (g_state.fix) {
                ESP_LOGI(TAG, "★★★ FIX=1 ✓ 定位成功! ★★★");
                ESP_LOGI(TAG, "  LAT=%.6f LNG=%.6f SAT=%d HDOP=%.1f TIME=%s",
                         g_state.lat, g_state.lng, g_state.sat,
                         g_state.hdop, g_state.gps_time);
                send_state_json();

                ESP_LOGI(TAG, "等待 10 秒后再次查询...");
                vTaskDelay(pdMS_TO_TICKS(10000));

                /* 第二次 STATUS? */
                uart_flush_rx();
                uart_send("STATUS?\r\n");
                pos = 0;
                deadline = esp_timer_get_time() / 1000 + 12000;
                while (esp_timer_get_time() / 1000 < deadline && pos < (int)sizeof(resp) - 1) {
                    int len = uart_read_bytes(UART_PORT, (uint8_t *)&resp[pos],
                                              sizeof(resp) - 1 - pos, pdMS_TO_TICKS(2000));
                    if (len > 0) { pos += len; deadline = esp_timer_get_time() / 1000 + 2000; }
                    else if (pos > 0) break;
                }
                resp[pos] = '\0';
                if (pos > 0) {
                    line = resp;
                    while (line && *line) {
                        next = strchr(line, '\n');
                        if (next) { *next = '\0'; char *e = line + strlen(line) - 1; if (e >= line && *e == '\r') *e = '\0'; }
                        if (strlen(line) > 2) parse_line(line);
                        line = next ? (next + 1) : NULL;
                    }
                    send_state_json();
                }
                break;
            }
        } else {
            ESP_LOGW(TAG, "STATUS? 无响应 (12秒超时)");
        }

        /* 等待 10 秒后重试 */
        ESP_LOGI(TAG, "等待 %d 秒后重试...", GPS_RETRY_INTERVAL_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(GPS_RETRY_INTERVAL_MS));
    }

    uint64_t total_ms = esp_timer_get_time() / 1000 - start_ms;
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  GPS 轮询结束 耗时:%llu秒 查询:%d次 结果:%s",
             total_ms / 1000, status_count, g_state.fix ? "FIX=1 ✓" : "FIX=0 ✗");
    ESP_LOGI(TAG, "  LAT=%.6f LNG=%.6f SAT=%d HDOP=%.1f TIME=%s",
             g_state.lat, g_state.lng, g_state.sat, g_state.hdop,
             strlen(g_state.gps_time) > 0 ? g_state.gps_time : "(无)");
    ESP_LOGI(TAG, "============================================");

    return g_state.fix;
}
