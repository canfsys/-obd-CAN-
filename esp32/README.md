# ESP32-S3 车载终端 — 多通道 CAN 数据采集

基于 **ESP32-S3-WROOM-1-N8R2** 的多通道 CAN 总线数据采集与存储终端，面向车载环境设计。支持内置 TWAI 控制器 + 外置 MCP2515 双通道 CAN 采集、SD 卡 CSV 日志、GPS 定时轮询、电池电压监测，以及完整的 light/deep sleep 低功耗电源管理。

## 功能特性

- 🚗 **双通道 CAN 采集**
  - CAN1: ESP32-S3 内置 TWAI 控制器 (125kbps) + SIT1145 收发器
  - CAN2: MCP2515 外置控制器 (SPI) + SIT1145 收发器
- 📦 **PSRAM 双缓冲区** — 4×64KB 缓冲区，CAN 帧满后通过共享信号量异步落盘，不丢帧
- 💾 **SD 卡 CSV 日志** — 按组件自动建目录、文件自动编号 (can_0001.csv) 并追加写入
- 🔋 **三级电源管理** — NORMAL → STANDBY → SLEEP (light sleep 3 次定时器唤醒后转 deep sleep)
- 🛰️ **GPS 定时轮询** — 进入 sleep 前设置 3 分钟定时器，唤醒后通过 UART1 查询 GPS 状态
- 📡 **4G 模块接口** — UART1 私有协议 + 复位控制 (开发中)
- 🌡️ **温度/电压监测** — DS18B20 单总线温度 + ADC 电池电压检测
- ⚡ **多核任务架构** — CAN 接收任务绑核最高优先级，SD 写入任务最低优先级防阻塞
- 🔋 **低功耗唤醒** — GPIO1 EXT1 高电平唤醒 + RTC 定时器自主唤醒

## 硬件要求

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3-WROOM-1-N8R2 (8MB Flash / 2MB PSRAM) |
| CAN1 | ESP TWAI (GPIO9/10) + SIT1145AQ 收发器 (SPI 配置) |
| CAN2 | MCP2515 (SPI) + SIT1145AQ 收发器 |
| 存储 | MicroSD 卡 (SDMMC 4-bit 模式) |
| 外设 | 4G 模块 (UART1)、GPS 模块 (UART1)、DS18B20、电池 |

## 引脚分配

| 功能 | GPIO | 说明 |
|------|:----:|------|
| UART0_TX / UART0_RX | 43 / 44 | 调试串口 (115200) |
| USB D- / D+ | 19 / 20 | USB Serial/JTAG 烧录 |
| SPI_MISO / MOSI / SCK | 16 / 17 / 18 | 共享 SPI2 总线 |
| SIT1145-1_CS | 5 | CAN1 收发器配置 (1MHz, mode1) |
| SIT1145-2_CS | 6 | CAN2 收发器配置 (1MHz, mode1) |
| MCP2515_CS | 7 | 外置 CAN 控制器 (8MHz, mode0) |
| MCP2515_INT | 8 | 中断 (下降沿) |
| TWAI_TX / TWAI_RX | 10 / 9 | 内置 CAN 控制器 |
| SD_CLK/CMD/D0~D3 | 14/15/2/4/12/13 | SDMMC 4-bit |
| SD_PWR_EN | 38 | SD 卡电源使能 |
| 4G_TX / 4G_RX | 41 / 42 | 4G 模块 UART1 |
| 4G_RESET | 47 | 4G 模块复位 |
| 3.8V_EN | 21 | 4G 模块/外部电源使能 |
| BAT_ADC / BAT_MEAS_EN | 11 / 39 | 电池电压检测 |
| DS18B20 | 40 | 温度传感器 |
| WAKE_IN | 1 | GPIO1 EXT1 高电平唤醒 |

## 软件要求

- **ESP-IDF v5.3.1**（其他 v5.x 版本理论上兼容）
- 目标芯片: `esp32s3`
- 需启用 PSRAM（Quad SPI 模式，`CONFIG_SPIRAM_MODE_QUAD`）

## 构建与烧录

```bash
# 设置 ESP-IDF 环境后
idf.py set-target esp32s3
idf.py menuconfig    # 按需调整 (见下方注意事项)
idf.py build
idf.py -p COM17 flash   # 替换为你的串口号
idf.py monitor          # 或使用串口助手连接 UART0 (115200-8N1)
```

### ⚠️ 构建注意事项

1. **PSRAM 必须为 Quad SPI 模式**（N8R2 模组），否则启动报错
2. **Flash 大小需设为 8MB**（`idf.py menuconfig` → Serial flasher config）
3. 调试串口为 **UART0 (GPIO43/44)**，USB Serial/JTAG 仅用于烧录

## 项目结构

```
├── CMakeLists.txt              # EXTRA_COMPONENT_DIRS=components
├── main/
│   └── main.c                  # 主入口: 唤醒判断 + 初始化 + 4 任务
└── components/                 # 自定义组件 (14 个)
    ├── board_config/           # 板级引脚定义
    ├── spi_bus/                # SPI2_HOST 共享总线 + 设备名映射
    ├── sit1145/                # SIT1145AQ CAN 收发器驱动
    ├── mcp2515/                # MCP2515 CAN 控制器驱动 (含重试)
    ├── twai/                   # ESP TWAI CAN 驱动封装 (Alert 中断)
    ├── sd_card/                # SD 卡 FATFS 挂载/读写
    ├── sd_logger/              # SD 卡 CSV 日志写入器
    ├── can_buffer/             # PSRAM 双缓冲区管理器
    ├── can_parser/             # CAN 数据帧解析器
    ├── modem_4g/               # 4G 模块 (UART + AT 命令)
    ├── ds18b20/                # 单总线温度传感器
    ├── adc/                    # 电池电压检测
    ├── power_manager/          # 电源管理 (三种模式 + GPIO 唤醒)
    └── gps_poller/             # GPS 轮询器
```

## 多任务架构

| 任务 | 绑核 | 优先级 | 触发方式 | 功能 |
|------|:----:|:------:|----------|------|
| `mcp2515_task` | Core0 | 24 | GPIO 下降沿 ISR | SPI 读 MCP2515 → 缓冲区 + 解析 |
| `twai_task` | Core1 | 24 | TWAI Alert(RXI) 中断 | 读 TWAI RX FIFO → 缓冲区 + 解析 |
| `sd_write_task` | Core1 | 1 | 共享信号量 | 缓冲区满 → 写 SD 卡 CSV |
| `sensor_task` | Core1 | 5 | `vTaskDelay(1000)` | 温度/电压打印 + 电源空闲检测 |

## 相关文档

- [PROJECT_SUMMARY.md](PROJECT_SUMMARY.md) — 硬件连接、架构、踩坑记录、版本记录
- [WAKEUP_CONFIG_SUMMARY.md](WAKEUP_CONFIG_SUMMARY.md) — 电源管理 / 唤醒配置详细说明

## 许可证

本项目基于 [GPL-3.0](LICENSE) 许可证开源。

> ⚠️ **免责声明**: 本项目为个人开发的参考实现，硬件涉及 CAN 总线通信，实际部署前请充分测试并评估安全性。
