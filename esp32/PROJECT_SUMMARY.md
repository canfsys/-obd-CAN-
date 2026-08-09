# ESP32-S3 车载终端工程 — 项目摘要

> **芯片型号:** ESP32-S3-WROOM-1-N8R2  
> **开发环境:** ESP-IDF v5.3.1  
> **目标平台:** ESP32-S3 (esp32s3)  
> **最后更新:** 2026-07-21

---

## 1. 硬件连接

### 1.1 调试串口
- **UART0 (GPIO43 TX, GPIO44 RX)**: 115200-8N1，所有调试信息输出
- **USB Serial/JTAG (GPIO19/20)**: 仅用于固件烧录

### 1.2 完整引脚分配

| 功能 | 组件 | GPIO | 说明 |
|------|------|:----:|------|
| **USB_D- / USB_D+** | 内置 | 19 / 20 | USB Serial/JTAG (烧录用) |
| **UART0_TX / UART0_RX** | 调试串口 | 43 / 44 | 控制台输出 (115200) |
| **SPI_MISO** | spi_bus | 16 | 共享 SPI 总线 |
| **SPI_MOSI** | spi_bus | 17 | |
| **SPI_SCK** | spi_bus | 18 | |
| **SIT1145-1_CS** | sit1145 (CAN1) | 5 | TWAI 路收发器 (1MHz, mode1) |
| **SIT1145-2_CS** | sit1145 (CAN2) | 6 | MCP2515 路收发器 (1MHz, mode1) |
| **MCP2515_CS** | mcp2515 | 7 | 外置 CAN 控制器 (8MHz, mode0) |
| **MCP2515_INT** | mcp2515 | 8 | 中断引脚 (下降沿触发) |
| **TWAI_TX** | twai | 10 | ESP32-S3 内置 CAN 控制器 TX |
| **TWAI_RX** | twai | 9 | ESP32-S3 内置 CAN 控制器 RX |
| **SD_CLK/CMD/D0/D1/D2/D3** | sd_card | 14/15/2/4/12/13 | SDMMC 4-bit 模式 |
| **SD_PWR_EN** | sd_card | 38 | SD 卡电源使能 |
| **4G_TX / 4G_RX** | modem_4g | 41 / 42 | 4G 模块 UART1 (私有协议) |
| **4G_RESET** | modem_4g | 47 | 4G 模块复位 |
| **3.8V_EN** | 电源控制 | 21 | 4G 模块/外部电源使能 |
| **BAT_ADC** | adc | 11 | 电池电压检测 (ADC2_CH0) |
| **BAT_MEAS_EN** | adc | 39 | ADC 通道开关 |
| **DS18B20** | ds18b20 | 40 | 单总线温度传感器 |
| **WAKE_IN** | 外部中断 | 1 | GPIO1 EXT1 高电平唤醒 |
| **USB_DN / USB_DP** | USB | 19 / 20 | 内置 USB Serial/JTAG |

---

## 2. CAN 通信架构

| CAN 总线 | 控制器 | 收发器 | 连接方式 |
|----------|--------|--------|----------|
| **CAN1** | ESP TWAI (内置) | SIT1145-1 (CS=GPIO5) | TWAI TX=GPIO10, RX=GPIO9 → SIT1145-1 |
| **CAN2** | MCP2515 (SPI, CS=GPIO7) | SIT1145-2 (CS=GPIO6) | MCP2515 → SIT1145-2 |

### SPI 总线共享设备 (SPI2_HOST)

| 设备 | CS | 速度 | SPI mode | 用途 |
|------|:--:|:----:|:--------:|------|
| SIT1145-1 | GPIO5 | 1MHz | mode 1 | 配置 TWAI 路收发器 |
| SIT1145-2 | GPIO6 | 1MHz | mode 1 | 配置 MCP2515 路收发器 |
| MCP2515 | GPIO7 | 8MHz | mode 0 | CAN 控制器通信 |

> ESP-IDF 驱动会分别为每个设备独立管理时序，切换 CS 时自动切换时钟速度。

---

## 3. 多任务架构

### 3.1 任务分配

| 任务 | 绑核 | 优先级 | 触发方式 | 功能 |
|------|:----:|:------:|----------|------|
| `mcp2515_task` | **Core0** | **最高 (24)** | GPIO 下降沿 ISR → Task Notification | SPI 读 MCP2515 → PSRAM 缓冲区 + 解析 |
| `twai_task` | **Core1** | **最高 (24)** | TWAI Alert(RXI) 硬件中断 | 读 TWAI RX FIFO → PSRAM 缓冲区 + 解析 |
| `sd_write_task` | Core1 | 最低 (1) | 共享计数信号量永久阻塞 | 缓冲区满 → 写入 SD 卡 CSV 文件 |
| `sensor_task` | Core1 | 5 | `vTaskDelay(1000)` | 温度/电压/状态打印 + 电源管理空闲检测 |

### 3.2 数据流

```
CAN 数据帧到达
    │
    ├→ mcp2515_task / twai_task 收到
    │   ├→ can_buffer_write_csv()      ← PSRAM 双缓冲区 (所有数据备份到 SD)
    │   └→ can_parser_process()        ← 解析关心 ID, 更新 g_state
    │
    └→ PSRAM 缓冲区满 (64KB)
        └→ 共享信号量通知 sd_write_task
            └→ 追加写入 SD 卡 CSV 文件
```

---

## 4. 工程结构 (组件化)

```
sample_project/
├── CMakeLists.txt                 # EXTRA_COMPONENT_DIRS=components
├── sdkconfig
├── components/                    # 自定义组件目录 (14 个)
│   ├── board_config/              # 板级引脚定义
│   ├── spi_bus/                   # SPI2_HOST 共享总线 + 设备名映射
│   ├── sit1145/                   # SIT1145AQ CAN 收发器驱动
│   ├── mcp2515/                   # MCP2515 CAN 控制器驱动 (含重试)
│   ├── twai/                      # ESP TWAI CAN 驱动封装 (125kbps, Alert)
│   ├── sd_card/                   # SD 卡 FATFS 挂载/读写
│   ├── sd_logger/                 # SD 卡 CSV 日志写入器 (自动编号+追加)
│   ├── can_buffer/                # PSRAM 双缓冲区管理器 (降级: 内部 RAM)
│   ├── can_parser/                # CAN 数据帧解析器 + GPS 状态变量
│   ├── modem_4g/                  # 4G 模块 (UART + AT 命令)
│   ├── ds18b20/                   # 单总线温度传感器
│   ├── adc/                       # 电池电压检测
│   ├── power_manager/             # 电源管理 (三种模式 + GPIO 唤醒)
│   └── gps_poller/                # GPS 轮询器 (UART1 收发 + KEY=VALUE 解析)
├── main/
│   ├── CMakeLists.txt
│   └── main.c                     # 主入口: 唤醒判断 + 初始化 + 4 任务
├── WAKEUP_CONFIG_SUMMARY.md
└── PROJECT_SUMMARY.md
```

---

## 5. 各组件 API 速览

### board_config
```c
#include "board_config.h"
// 引脚宏: PIN_SPI_MOSI, PIN_SIT1_CS, PIN_TWAI_TX, PIN_WAKE_IN, PIN_UART0_TX 等
```

### spi_bus
```c
spi_bus_init_board(mosi, miso, sck);                        // 初始化 SPI2_HOST
spi_bus_add_device_custom(cs, clock_hz, mode, &handle);     // 添加 SPI 设备
spi_bus_del_device(handle);                                  // 删除 SPI 设备 (light sleep 唤醒)
spi_bus_set_device_name(handle, "SIT1145-1");                // SPI 日志带设备名
spi_bus_write_reg(handle, addr, data);                      // 写 16-bit 寄存器
spi_bus_read_reg(handle, addr, &data);                      // 读 16-bit 寄存器
spi_bus_transfer(handle, tx, rx, bit_len);                  // 原始全双工传输 + HEX 日志
```

### sit1145
```c
sit1145_init(SIT1145_DEV_1, handle);           // 初始化 (写入0x26→0x20, 三寄存器验证)
sit1145_verify_chip_id(SIT1145_DEV_1);          // 验证芯片 ID (0x74)
sit1145_read_reg(dev, reg, &val);               // 读寄存器
sit1145_write_reg(dev, reg, data);              // 写寄存器 (地址编码: (reg<<1)|R/W)
```

### mcp2515
```c
mcp2515_init(handle, int_pin);                  // 初始化 (重试 5 次)
mcp2515_register_task(task_handle);             // 注册通知任务
mcp2515_send_frame(&frame);                     // 发送 (0x40 装载 + 0x81 快速发送)
mcp2515_receive_frame(&frame);                  // 接收 (0xB0→0x90→0x05)
mcp2515_has_interrupt();                        // 检查 INT 引脚电平
```

### twai
```c
twai_driver_init(TX_PIN, RX_PIN);               // 初始化 (125kbps, rx_queue=128)
twai_driver_start();                             // 启动 TWAI + Alert(RXI+DOI)
twai_driver_stop();                              // 停止
twai_driver_deinit();                            // 卸载驱动 (light sleep 进入前)
twai_driver_transmit(&frame, ms);                // 发送
twai_driver_receive(&frame, ms);                 // 接收 (带日志)
twai_driver_wait_rx(timeout_ms);                 // Alert 阻塞等待 RX 数据
```

### sd_card
```c
sd_card_init(pwr_en_pin);                       // 挂载 SD 卡
sd_card_deinit();                               // 卸载
sd_card_write_file(path, data, len);            // 写文件
sd_card_read_file(path, buf, size, &out);       // 读文件
```

### sd_logger
```c
sd_logger_init("sit1145");                      // 创建新文件 + CSV 头
sd_logger_write("mcp2515", data, len);          // 追加写入
// 目录: /sdcard/sit1145/can_0001.csv
// 格式: timestamp_ms,can_id,dlc,d0~d7,extended,rtr
```

### can_buffer
```c
can_buffer_init(&ctx, "mcp2515");               // 分配 2 缓冲区 (PSRAM 优先, 降级内部 RAM)
can_buffer_write_csv(&ctx, id, dlc, data, ...);  // 写 CSV 行
can_buffer_get_full(&ctx, &buf, &size);          // 获取满缓冲区
can_buffer_release_full(&ctx);                   // 释放满缓冲区
```

### can_parser
```c
can_parser_process(can_id, data, dlc);           // 解析 CAN 帧, 更新 g_state
// g_state 包含: lat/lng/speed/net/csq/fix/sat/hdop/altitude/height/gps_time
//               voltage/fuel/temperature/rpm/throttle/车门/车窗/手刹/点火 等
```

### modem_4g
```c
modem_4g_init(tx, rx, reset, baud);             // 初始化 UART1
modem_4g_send_at("AT\r\n", resp, size, ms);     // 发送 AT 命令
```

### gps_poller
```c
gps_poller_uart_init();                          // GPIO21=高, 初始化 UART1
gps_poller_run_cycle();                          // 发送 STATUS?, 解析 KEY=VALUE, FIX=1 发 JSON
gps_poller_uart_deinit();                        // 关闭 UART1, GPIO21=低
```

### power_manager
```c
power_manager_init();                            // 初始化为 NORMAL 模式
power_manager_tick(now_ms);                      // 每秒检查空闲超时 (1min)
power_manager_should_standby();                  // 是否应进入待机
power_manager_enter_standby();                   // 进入 light sleep (3min GPS)
power_manager_enter_sleep();                     // 进入 deep sleep (仅 GPIO1)
power_manager_is_auto_wake();                    // 是否为定时器自主唤醒
power_manager_set_mode(mode);                    // 设置电源模式
```

---

## 6. sdkconfig 关键配置

```c
// 控制台: UART0 (GPIO43/44)
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_NUM=0

// Flash 8MB & PSRAM
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_QUAD=y
CONFIG_SPIRAM_SPEED_40M=y

// CPU 主频
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
```

---

## 7. 电源管理模式 (核心)

### 三种模式

| 模式 | 描述 | 触发条件 | 4G 电源 | GPIO1 | GPIO42 |
|------|------|----------|:-------:|:-----:|:------:|
| **NORMAL** | 全速运行 | 上电 / GPIO 唤醒 | 高 | 输入 | UART1 |
| **STANDBY** | Light sleep | 1min 无 CAN 报文 | 高 | EXT1 唤醒 | IO MUX 唤醒 |
| **SLEEP** | Deep sleep | 定时器唤醒 3 次 | 低 | EXT1 唤醒 | 高阻态 |

### Light sleep 流程

```
正常模式 → 1分钟无CAN → light sleep (3min)
  3min 到 → [定时器唤醒]
           → GPIO21=高 (保持)
           → 初始化 UART1
           → 发送 STATUS?
           → 解析 KEY=VALUE (FIX=1 → 更新 g_state + 发 JSON)
           → 2 分钟窗口结束
           → 回到 light sleep (s_wake_count++)
           → 3 次后进 deep sleep
```

### Deep sleep 流程

```
deep sleep → 3min 到 → 重启
           → GPIO21=高
           → GPS 轮询 2min
           → GPIO21=低
           → deep sleep (循环)
```

### 唤醒源

| 唤醒源 | 作用阶段 | 唤醒原因码 | 操作 |
|--------|----------|:----------:|------|
| GPIO1 (EXT1) | Light/Deep sleep | 1 | 全初始化外设 |
| GPIO42 (IO MUX) | Light sleep only | 2 | 全初始化外设 |
| RTC 定时器 | Light sleep only | 4 | GPS 轮询 |

---

## 8. 初始化日志 (当前版本)

```
I (786) MAIN: 外部唤醒/上电: 全初始化
I (786) MAIN: ===== 全初始化 =====
I (846) SPI_BUS: 初始化 SPI2_HOST 总线: MOSI=17, MISO=16, SCK=18
I (846) SPI_BUS: 总线初始化成功
I (846) SPI_BUS: 设备 (CS=5) 添加成功, 速度=1000000Hz, mode=1
I (846) SPI_BUS: 设备 (CS=6) 添加成功, 速度=1000000Hz, mode=1
I (856) SPI_BUS: 设备 (CS=7) 添加成功, 速度=8000000Hz, mode=0
I (866) SIT1145: SIT1145-1 写入: reg[0x26]=0x02
I (876) SIT1145: SIT1145-1 写入: reg[0x20]=0x32
I (896) SIT1145: SIT1145-1 读回验证: reg[0x26]=0x02, reg[0x20]=0x32, reg[0x22]=0xA8
I (916) SIT1145: SIT1145-1 验证通过 ✓
I (926) SIT1145: SIT1145-2 写入: reg[0x26]=0x05
I (966) SIT1145: SIT1145-2 读回验证: reg[0x26]=0x05, reg[0x20]=0x32, reg[0x22]=0xA8
I (1006) SIT1145: SIT1145-2 验证通过 ✓
I (1026) SIT1145: SIT1145-1 Chip ID = 0x74 ✓
I (1036) SIT1145: SIT1145-2 Chip ID = 0x74 ✓
I (1226) MCP2515: MCP2515 验证 (retry=1, CANSTAT=0x00, OPMOD=0 Normal ✓)
I (1266) TWAI: TWAI 驱动安装成功 (125kbps)
I (1266) TWAI: TWAI 通信启动, Alert 已配置 (RXI + DOI)
I (1516) SD_CARD: SD 卡初始化成功
I (2886) MAIN: PSRAM: 2097152 bytes
I (2906) CAN_BUF: mcp2515: 双缓冲区初始化完成 (PSRAM, 2×65536 bytes)
I (3956) MAIN: ===== 全初始化完成 =====

--- 进入 light sleep 后 ---
I (64426) POWER_MGR: 进入 light sleep (3min GPS, GPIO1/42 唤醒)
I (64446) POWER_MGR: 唤醒 原因=4(RTC定时器)
I (64475) MAIN: 定时器唤醒 (wake_count=0/3)
I (64475) GPS_POLL: ===== GPS 轮询: UART1 初始化 =====
I (64585) GPS_POLL: --- 第 1 次 STATUS? (已用 0 秒) ---
```

---

## 9. PSRAM 缓冲区分配

| 缓冲区 | 大小 | 数量 | 总占用 | 分配位置 |
|--------|:----:|:----:|:------:|----------|
| MCP2515 双缓冲区 | 64KB | 2 | 128KB | PSRAM (降级: 16KB 内部 RAM) |
| TWAI 双缓冲区 | 64KB | 2 | 128KB | PSRAM (降级: 16KB 内部 RAM) |
| **合计** | | **4个** | **256KB** | `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` |

---

## 10. 版本变更记录

| 日期 | 变更 | 说明 |
|------|------|------|
| 2026-07-15 | 初始组件化 | 9 个组件, 单任务轮询 |
| 2026-07-15 | 多任务架构 | 4 任务 Core0+Core1, PSRAM 双缓冲区, SD CSV 日志 |
| 2026-07-18 | **电源管理** | power_manager 组件: 三种模式 |
| 2026-07-18 | GPIO 唤醒调试 | Light sleep 唤醒问题排查 |
| 2026-07-18 | RTC GPIO API | rtc_gpio_init + EXT1 + pd_config |
| 2026-07-18 | Deep sleep 唤醒 | deep sleep 能正常唤醒 |
| 2026-07-18 | UART0 调试串口 | 控制台从 USB_JTAG 切换到 UART0 |
| 2026-07-20 | **GPS 轮询** | gps_poller 组件 |
| 2026-07-20 | 唤醒类型判断 | RTC_DATA_ATTR 标志 + app_main 判断 |
| 2026-07-20 | MCP2515 重试 | 复位重试 5 次 |
| 2026-07-20 | Light sleep 唤醒修复 | 删除 GPIO42 IO MUX 冲突 |
| 2026-07-21 | **完整 sleep 循环** | light sleep 3min × 3 → deep sleep 3min GPS |
| 2026-07-21 | wake_count 简化 | 单个 RTC 变量控制 light→deep 切换 |
| 2026-07-21 | STATUS? 直发 | GPS 轮询不再等待 GPS=OK |
| 2026-07-21 | SPI 休眠标志 | s_periph_initialized 防止 GPS 轮询时误休眠 |
| 2026-07-21 | GPIO42 唤醒恢复 | IO MUX 高电平唤醒重新加入 |
| 2026-07-21 | GPS 超时修复 | uart_read_bytes 200ms→2000ms |

---

## 11. 踩坑记录

### 11.1 PSRAM 模式错误
- N8R2 模组 PSRAM 是 **Quad SPI** 模式, 非 Octal

### 11.2 SIT1145 地址编码
- R/W 位在 **LSB (bit0)**: `ADDR_W = (reg<<1)|0`, `ADDR_R = (reg<<1)|1`

### 11.3 SIT1145 SPI mode
- SIT1145 使用 **mode 1** (下降沿采样)

### 11.4 Light sleep 无法唤醒 (已解决)
- 原因: GPIO1 未配置 `rtc_gpio_` API + `esp_sleep_pd_config(RTC_PERIPH, ON)`
- 原因: ESP-IDF 默认 `sleep: Configure to isolate all GPIO pins`
- 解决: `rtc_gpio_init + gpio_sleep_sel_dis + EXT1 + pd_config`

### 11.5 IO MUX 与 EXT1 冲突 (已解决)
- 同时使用 `esp_sleep_enable_gpio_wakeup()` 和 `esp_sleep_enable_ext1_wakeup()` 导致立即唤醒
- 解决: 只能二选一，GPIO1 用 EXT1，GPIO42(非 RTC) 用 IO MUX

### 11.6 Deep sleep 5 分钟自动重启 (已解决)
- 原因: Light sleep 的定时器残留到 deep sleep
- 解决: `esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER)`

### 11.7 Light sleep 唤醒后 SPI 冲突 (已解决)
- 原因: `spi_bus_init_board()` 重复调用
- 解决: wake_reinit 中跳过总线初始化, 删除旧设备再添加

### 11.8 GPS 轮询 UART 收不到数据 (已解决)
- 原因: `uart_read_bytes()` 每次轮询超时 200ms 太短, 模块首字节在 200ms 后才到达
- 解决: 超时改为 2000ms

---

## 12. 常用命令

```bash
# 编译
idf.py build

# 烧录 (USB 线, 替换为你的串口号)
idf.py -p COM17 flash

# 查看 UART0 调试输出 (USB 转串口模块接 GPIO43/44)
# 串口助手: COMxx 115200-8N1
```

---

## 13. TO-DO / 后续改进

- [x] 基础 CAN 收发 (MCP2515 + TWAI)
- [x] 多任务架构 (4 任务)
- [x] SD 卡 CSV 日志
- [x] 三种电源模式 (NORMAL / STANDBY / SLEEP)
- [x] GPIO1 EXT1 高电平唤醒
- [x] GPS 轮询 (每 3 分钟, STATUS? 查询)
- [x] Light→Deep sleep 切换 (3 次后)
- [x] Flash 大小 8MB 修正
- [ ] DS18B20 温度传感器 (待硬件连接后验证)
- [ ] CPU 主频提升到 240MHz
- [ ] 4G 模块 AT 命令通信完善
- [ ] 外部 RTC 时间同步 (用 GPS TIME 校准)
- [ ] WiFi/BLE 功能测试
- [ ] 解析串口数据逻辑优化