/**
 * @file can_parser.c
 * @brief CAN 数据帧解析器实现
 *
 * 根据 CAN ID 识别并解析关心的数据帧.
 * 解析结果更新到 g_state 中的对应字段.
 *
 * ID 映射示例 (可根据实际 CAN 协议修改):
 *   0x100 - 发动机状态 (转速、水温、油门)
 *   0x200 - 车身控制 (车门、车窗、灯光)
 *   0x300 - 底盘状态 (车速、刹车、手刹)
 *   0x400 - 舒适系统 (空调、安全带、座椅)
 *   0x500 - 仪表信息 (里程、燃油)
 */

#include "can_parser.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "CAN_PARSER";

/* 全局系统状态变量定义 (满足 can_parser.h 中的 extern) */
system_state_t g_state = {0};

void can_parser_process(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    /* 至少需要 2 字节数据才解析 */
    if (!data || dlc < 1) return;

    /* 更新最后接收 CAN 报文的时间戳 (用于电源管理空闲检测) */
    g_state.last_can_ms = esp_timer_get_time() / 1000;

    switch (can_id) {

    /* ================================================
     * ID 0x100: 发动机状态
     *   data[0-1]: 发动机转速 (rpm, big-endian)
     *   data[2]:   油门深度 (0-100%)
     *   data[3]:   冷却液温度 (°C)
     * ================================================ */
    case 0x100:
        if (dlc >= 4) {
            g_state.rpm      = ((uint16_t)data[0] << 8) | data[1];
            g_state.throttle  = data[2];
            /* data[3] = 水温, 暂不处理 */
            ESP_LOGD(TAG, "0x100: rpm=%d, throttle=%d%%", g_state.rpm, g_state.throttle);
        }
        break;

    /* ================================================
     * ID 0x200: 车身控制
     *   data[0] bit0~4: 5 个车门 (0=关, 1=开)
     *   data[1] bit0~3: 4 个车窗 (0=关, 1=开)
     *   data[2] bit0:   灯光 (0=关, 1=近光, 2=远光)
     * ================================================ */
    case 0x200:
        if (dlc >= 3) {
            /* 车门: 5 位 0/1 字符串 */
            snprintf(g_state.door, sizeof(g_state.door), "%d%d%d%d%d",
                     (data[0] >> 4) & 0x01,  /* 左后 */
                     (data[0] >> 3) & 0x01,  /* 右后 */
                     (data[0] >> 2) & 0x01,  /* 右前 */
                     (data[0] >> 1) & 0x01,  /* 左前 */
                     data[0] & 0x01);        /* 尾门 */

            /* 车窗: 4 位 0/1 字符串 */
            snprintf(g_state.window, sizeof(g_state.window), "%d%d%d%d",
                     (data[1] >> 3) & 0x01,
                     (data[1] >> 2) & 0x01,
                     (data[1] >> 1) & 0x01,
                     data[1] & 0x01);

            g_state.light = data[2];  /* 0=关 1=近光 2=远光 */

            ESP_LOGD(TAG, "0x200: door=%s, window=%s, light=%d",
                     g_state.door, g_state.window, g_state.light);
        }
        break;

    /* ================================================
     * ID 0x300: 底盘状态
     *   data[0-1]: 轮上速度 (km/h, big-endian *10)
     *   data[2]:   手刹 (0=放下, 1=拉起)
     *   data[3]:   刹车 (0=松开, 1=踩下)
     *   data[4]:   离合器 (0=松开, 1=踩下)
     * ================================================ */
    case 0x300:
        if (dlc >= 5) {
            g_state.wheel_speed = (float)(((uint16_t)data[0] << 8) | data[1]) / 10.0f;
            g_state.handbrake   = data[2] ? 1 : 0;
            g_state.brake       = data[3] ? 1 : 0;
            g_state.clutch      = data[4] ? 1 : 0;
            ESP_LOGD(TAG, "0x300: speed=%.1f, brake=%d, clutch=%d",
                     g_state.wheel_speed, g_state.brake, g_state.clutch);
        }
        break;

    /* ================================================
     * ID 0x400: 舒适系统
     *   data[0]:   安全带 (0=未系, 1=系上)
     *   data[1]:   副驾有人 (0=无, 1=有)
     *   data[2]:   空调AC (0=关, 1=开)
     * ================================================ */
    case 0x400:
        if (dlc >= 3) {
            g_state.seatbelt  = data[0] ? 1 : 0;
            g_state.passenger = data[1] ? 1 : 0;
            g_state.ac        = data[2] ? 1 : 0;
            ESP_LOGD(TAG, "0x400: seatbelt=%d, passenger=%d, ac=%d",
                     g_state.seatbelt, g_state.passenger, g_state.ac);
        }
        break;

    /* ================================================
     * ID 0x500: 仪表信息
     *   data[0-1]: 燃油 (0-100%)
     *   data[4-7]: 总里程 (km, big-endian uint32)
     * ================================================ */
    case 0x500:
        if (dlc >= 5) {
            g_state.fuel    = data[0];
            g_state.mileage = (float)(
                ((uint32_t)data[4] << 24) |
                ((uint32_t)data[5] << 16) |
                ((uint32_t)data[6] << 8)  |
                data[7]) / 10.0f;
            ESP_LOGD(TAG, "0x500: fuel=%d%%, mileage=%.1f km",
                     g_state.fuel, g_state.mileage);
        }
        break;

    /* ================================================
     * ID 0x600: 点火/熄火状态
     *   data[0]: 点火 (0=熄火, 1=点火)
     * ================================================ */
    case 0x600:
        if (dlc >= 1) {
            g_state.ignition = data[0] ? 1 : 0;
            strcpy(g_state.carState, g_state.ignition ? "online" : "offline");
            ESP_LOGI(TAG, "0x600: ignition=%d, carState=%s",
                     g_state.ignition, g_state.carState);
        }
        break;

    default:
        /* 未识别的 ID, 静默忽略 */
        break;
    }
}