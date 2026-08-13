# OBD-CAN 车载终端 — 全链路开源项目

一套面向车联网场景的 **车载数据采集与远程监控系统**，从 PCB 硬件设计、CAN 总线数据采集、4G 上传、云端存储到微信小程序远程监控，全链路开源。

## 📦 仓库组成

| 目录 | 说明 | 核心技术 |
|------|------|---------|
| [`esp32/`](esp32/README.md) | **ESP32-S3 车载终端固件** — 双通道 CAN 采集、GPS、SD 卡日志、低功耗电源管理 | C / ESP-IDF v5.3.1 / FreeRTOS |
| [`miniprogram/`](miniprogram/README.md) | **微信小程序** — 实时位置、轨迹回放、CAN 状态、历史图表、告警推送 | 微信云开发 / 云函数 |
| [`iot/`](iot/README.md) | **4G 物联网模块（银尔达 M100PG-DTU 固件版）** — 串口桥接脚本，HTTP 上报 + MQTT 下行指令 | Lua（银尔达平台） |
| [`hardware/`](hardware/README.md) | **PCB 硬件设计** — ESP32-S3 四层板（双 CAN、SD、4G、GPS、电源管理） | 嘉立创EDA专业版 / CERN-OHL-S-2.0 |

## 🏗️ 系统架构

```
┌─────────────────────────────── 车载端 ───────────────────────────────┐
│  ┌────────────┐    CAN总线   ┌──────────────┐   UART    ┌──────────┐  │
│  │ ESP32-S3   │◀────────────▶│ 双通道CAN采集 │◀─────────▶│ 4G 模块   │  │
│  │ 固件 esp32/ │  (TWAI+2515) │  GPS/SD/电源  │  串口协议  │ iot/     │  │
│  └────────────┘              └──────────────┘            └──────────┘  │
└───────────────────────────────────────────────────────────────────────┘
                                        │ HTTP POST (4G网络)
                                        ▼
                                ┌────────────────┐   轮询15s   ┌──────────┐
                                │ 云函数 upload  │◀─────────────│ 微信小程序 │
                                │ (云数据库存储)  │              │miniprogram│
                                └────────────────┘              └──────────┘
                                        │ 定时检测
                                        ▼
                                ┌────────────────┐
                                │ alertChecker    │──▶ notify ──▶ 订阅消息推送
                                └────────────────┘
```

**数据链路**：车辆 CAN 数据 → ESP32 解析 → 4G 模块 HTTP 上报 → 腾讯云开发数据库 → 小程序 15 秒轮询展示 → 异常离线/非法移动 → 微信订阅消息告警。

## 🚀 快速开始

```bash
# ESP32 固件（见 esp32/README.md）
cd esp32
idf.py set-target esp32s3
idf.py build && idf.py -p COM17 flash

# 小程序：微信开发者工具导入本仓库根目录，按 miniprogram/README.md 部署
# 4G 模块：银尔达平台上传 iot/espBridge.lua，按 iot/config.md 配置通道
```

## 📄 文档索引

| 文档 | 内容 |
|------|------|
| [esp32/PROJECT_SUMMARY.md](esp32/PROJECT_SUMMARY.md) | 硬件连接、多任务架构、踩坑记录、版本记录 |
| [esp32/WAKEUP_CONFIG_SUMMARY.md](esp32/WAKEUP_CONFIG_SUMMARY.md) | 电源管理 / 唤醒配置详解 |
| [hardware/README.md](hardware/README.md) | PCB 硬件设计、引脚定义、打样/贴片说明 |
| [miniprogram/DATA_DOC.md](miniprogram/DATA_DOC.md) | 数据字段定义、数据库集合、告警链路、云函数部署 |
| [iot/config.md](iot/config.md) | 4G 平台网络通道配置 |

## ⚠️ 部署须知

- 仓库中小程序端配置已**脱敏**（`YOUR_APPID`、`YOUR_ENV_ID`、`YOUR_TEMPLATE_ID` 等占位符），部署前需替换为自己的配置，详见 `miniprogram/README.md`
- ESP32 需使用 N8R2 模组（8MB Flash + 2MB PSRAM，Quad SPI），详见 `esp32/README.md`

## 📜 许可证

本项目基于 [GPL-3.0](LICENSE) 许可证开源。

> ⚠️ **免责声明**：本项目为个人开发的参考实现，硬件涉及 CAN 总线通信与车辆数据，实际部署前请充分测试并评估安全性。
