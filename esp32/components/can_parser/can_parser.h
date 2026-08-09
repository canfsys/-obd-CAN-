/**
 * @file can_parser.h
 * @brief CAN 数据帧解析器
 *
 * 根据 CAN ID 识别并解析关心的数据帧,
 * 将解析结果更新到系统状态变量 (g_state).
 *
 * 使用方式: 在 mcp2515_task / twai_task 收到帧后调用:
 *   can_parser_process(frame.id, frame.data, frame.dlc);
 */

#ifndef CAN_PARSER_H
#define CAN_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 系统状态结构体 (全局变量, 供各模块访问)
 *
 * 包含车辆所有状态字段, CAN 解析器将解析结果写入此结构.
 */
typedef struct {
    /* 设备标识 */
    char     deviceId[16];       /*!< 设备ID（固定 "car001"）              */

    /* GPS 定位 (由 4G 模块上报) */
    double   lat;                /*!< 纬度                                */
    double   lng;                /*!< 经度                                */
    float    speed;              /*!< GPS 速度 (km/h)                     */
    uint8_t  net;                /*!< 网络注册状态 (1=注册, 0=未注册)      */
    uint8_t  csq;                /*!< 4G 信号质量 (0-31)                   */
    uint8_t  gps;                /*!< GPS 开关 (1=开 0=关)                */
    uint8_t  fix;                /*!< 定位状态 (1=已定位 0=未定位)         */
    uint8_t  sat;                /*!< 卫星颗数                             */
    float    hdop;               /*!< HDOP 水平精度因子                    */
    float    altitude;           /*!< 海拔 (m)                            */
    float    height;             /*!< 天线高度 (m)                        */
    char     gps_time[24];       /*!< GPS 时间 "2026-07-20 22:20:14"       */

    /* 车辆状态 */
    float    voltage;            /*!< 电压 (V)                            */
    uint8_t  fuel;               /*!< 燃油 (%)                            */
    float    temperature;        /*!< 车内温度 (°C)                        */
    char     carState[16];       /*!< 车辆状态 (online/offline/lowpower/waiting) */

    /* 车身控制 (由 CAN 解析器更新) */
    char     door[8];            /*!< 5 个车门状态（如 "10000"）           */
    char     window[8];          /*!< 4 个车窗状态（如 "0000"）            */
    uint8_t  handbrake;          /*!< 手刹 (1=拉起 0=放下)                 */
    uint8_t  ignition;           /*!< 点火 (1=点火 0=熄火)                 */
    uint8_t  clutch;             /*!< 离合器 (1=踩下 0=松开)               */
    uint8_t  brake;              /*!< 刹车 (1=踩下 0=松开)                 */
    uint8_t  light;              /*!< 灯光 (0=关 1=近光 2=远光)            */
    uint8_t  ac;                 /*!< 空调AC (1=开 0=关)                   */
    uint8_t  seatbelt;           /*!< 主驾安全带 (1=系上 0=未系)           */
    uint8_t  passenger;          /*!< 副驾是否有人 (1=有 0=无)             */
    uint8_t  throttle;           /*!< 油门深度 (%)                         */
    uint16_t rpm;                /*!< 发动机转速 (r/m)                     */
    float    wheel_speed;        /*!< 轮上速度 (km/h)                      */
    float    mileage;            /*!< 总里程 (km)                          */

    /* 外设状态 */
    uint8_t  sit1145_1_init;     /*!< sit1145-1状态 (1=正常 0=故障)        */
    uint8_t  sit1145_2_init;     /*!< sit1145-2状态 (1=正常 0=故障)        */
    uint8_t  mcp2515_init;       /*!< mcp2515状态   (1=正常 0=故障)        */

    /* 电源管理 */
    uint64_t last_can_ms;        /*!< 最近一次收到 CAN 报文的时间戳 (ms)    */
} system_state_t;

/** 全局系统状态变量 (在 main.c 中定义) */
extern system_state_t g_state;

/**
 * @brief CAN 帧解析入口
 *
 * 根据 CAN ID 识别帧类型, 解析数据并更新 g_state.
 * 在 mcp2515_task / twai_task 的接收循环中调用.
 *
 * @param can_id  CAN 标准帧 ID (11-bit)
 * @param data    CAN 数据 (8 字节)
 * @param dlc     数据长度
 */
void can_parser_process(uint32_t can_id, const uint8_t *data, uint8_t dlc);

#ifdef __cplusplus
}
#endif

#endif /* CAN_PARSER_H */