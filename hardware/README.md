# OBD-CAN 车载终端 — 硬件设计

> **硬件许可证：** [CERN-OHL-S-2.0](LICENSE)（强 copyleft，衍生硬件必须同样开源）
> **EDA 工具：** 嘉立创EDA专业版（LCEDA Pro）
> **状态：** PCB 已设计完成，尚未量产

## 📋 技术规格

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3-WROOM-1-N8R2（8MB Flash / 2MB PSRAM，Quad SPI） |
| CAN1 | ESP32-S3 内置 TWAI 控制器 + SIT1145AQ 收发器（1MHz） |
| CAN2 | MCP2515 外置 CAN 控制器（SPI 8MHz）+ SIT1145AQ 收发器 |
| 供电 | 车载 12V 输入，含 3.8V_EN 电源控制 |
| 存储 | MicroSD 卡（SDMMC 4-bit 模式，独立电源使能） |
| 通信 | 4G 模块 UART1、GPS 模块 UART1（复用）、DS18B20 单总线 |
| 检测 | 电池电压 ADC（2:1 分压）+ 测量使能 |
| 电源管理 | Light sleep / Deep sleep，GPIO1 EXT1 高电平唤醒 |
| 板层 | 4 层板（含内层 G1/G2） |

## 📁 目录结构

```
hardware/
├── LICENSE                        # CERN-OHL-S-2.0 硬件开源许可证
├── schematic.pdf                  # 原理图（PDF 通用格式）
├── gerber/
│   ├── Gerber_PCB1.zip            # 完整 Gerber 打包（可直接打样）
│   ├── Gerber_TopLayer.GTL        # 顶层铜箔
│   ├── Gerber_BottomLayer.GBL     # 底层铜箔
│   ├── Gerber_InnerLayer1.G1      # 内层1
│   ├── Gerber_InnerLayer2.G2      # 内层2
│   ├── Gerber_*SilkscreenLayer    # 丝印层（顶/底）
│   ├── Gerber_*SolderMaskLayer    # 阻焊层（顶/底）
│   ├── Gerber_*PasteMaskLayer     # 锡膏层（顶/底）
│   ├── Gerber_BoardOutlineLayer.GKO  # 板框
│   ├── Drill_PTH/NPTH/Via.DRL     # 钻孔文件
│   ├── FlyingProbeTesting.json    # 飞针测试文件
│   └── PCB下单必读.txt
├── bom/
│   ├── BOM_Board1_PCB1.xlsx       # 物料清单（元器件/数量/封装）
│   └── PickAndPlace_PCB1.xlsx     # 贴片坐标文件（SMT 使用）
├── images/
│   ├── 3D_PCB1_TopLayer.png       # PCB 3D 顶面预览
│   └── 3D_PCB1_BottomLayer.png    # PCB 3D 底面预览
└── source/
    └── OBD_CAN_Module_v2.0.eprj2  # 嘉立创EDA专业版工程源文件（可二次编辑）
```

## 🔌 引脚分配

| 功能 | 组件 | GPIO | 说明 |
|------|------|:----:|------|
| UART0_TX / UART0_RX | 调试串口 | 43 / 44 | 控制台输出 (115200) |
| USB D- / D+ | 内置 | 19 / 20 | USB Serial/JTAG 烧录 |
| SPI_MISO / MOSI / SCK | spi_bus | 16 / 17 / 18 | 共享 SPI2 总线 |
| SIT1145-1_CS | sit1145 (CAN1) | 5 | TWAI 路收发器 (1MHz, mode1) |
| SIT1145-2_CS | sit1145 (CAN2) | 6 | MCP2515 路收发器 (1MHz, mode1) |
| MCP2515_CS | mcp2515 | 7 | 外置 CAN 控制器 (8MHz, mode0) |
| MCP2515_INT | mcp2515 | 8 | 中断引脚 (下降沿) |
| TWAI_TX / TWAI_RX | twai | 10 / 9 | ESP32-S3 内置 CAN |
| SD_CLK/CMD/D0~D3 | sd_card | 14/15/2/4/12/13 | SDMMC 4-bit |
| SD_PWR_EN | sd_card | 38 | SD 卡电源使能 |
| 4G_TX / 4G_RX | modem_4g | 41 / 42 | 4G 模块 UART1 |
| 4G_RESET | modem_4g | 47 | 4G 模块复位 |
| 3.8V_EN | 电源控制 | 21 | 4G 模块/外部电源使能 |
| BAT_ADC | adc | 11 | 电池电压检测 (ADC2_CH0) |
| BAT_MEAS_EN | adc | 39 | ADC 通道开关 |
| DS18B20 | ds18b20 | 40 | 单总线温度传感器 |
| WAKE_IN | 外部中断 | 1 | GPIO1 EXT1 高电平唤醒 |

## 🛠️ 使用说明

### 打样 PCB
1. 将 `gerber/Gerber_PCB1.zip` 直接上传至嘉立创下单助手 / 制板厂
2. 按 `PCB下单必读.txt` 中的要求下单

### 贴片生产（SMT）
1. 打开 `bom/BOM_Board1_PCB1.xlsx` 核对元器件
2. 使用 `bom/PickAndPlace_PCB1.xlsx` 贴片坐标文件
3. 配合飞针测试文件 `FlyingProbeTesting.json` 验证

### 二次开发（修改电路）
1. 用嘉立创EDA专业版打开 `source/OBD_CAN_Module_v2.0.eprj2` 工程文件
2. 修改后导出新的 Gerber / BOM / 原理图 PDF 更新到对应目录

## 📜 许可证

硬件设计基于 [CERN-OHL-S-2.0](LICENSE) 开源。固件与软件部分见仓库根目录 GPL-3.0 许可证。

> ⚠️ **免责声明**：本设计为个人开发参考，涉及车辆 CAN 总线与电气系统，实际使用前请充分测试并评估安全风险。
