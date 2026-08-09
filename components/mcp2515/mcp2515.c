/**
 * @file mcp2515.c
 * @brief MCP2515 SPI 接口 CAN 控制器驱动实现
 *
 * 通过 SPI 命令操作 MCP2515 内部寄存器.
 * 初始化序列严格按用户指定的寄存器配置执行.
 * 接收流程: 0xB0 读状态 → 0x90/0x94 读数据 → 0x05 位修改清中断.
 *
 * INT 引脚下降沿触发 GPIO 中断, ISR 通过 FreeRTOS Task Notification
 * 通知注册的任务, 实现微秒级响应.
 */

#include "mcp2515.h"
#include "board_config.h"
#include "spi_bus.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "MCP2515";

/* ---------- 模块内部变量 ---------- */
static spi_device_handle_t s_handle = NULL;  /* SPI 设备句柄          */
static int s_int_pin = -1;                    /* 中断引脚 (GPIO8)      */
static TaskHandle_t s_notify_task = NULL;     /* 接收通知的任务句柄    */
static bool s_isr_installed = false;          /* ISR 是否已安装        */

/* ====================================================================== */
/*  寄存器地址定义                                                         */
/* ====================================================================== */
#define REG_BFPCTRL     0x0C    /* RXnBF 引脚控制和状态                  */
#define REG_CANCTRL     0x0F    /* CAN 控制寄存器                        */
#define REG_CANSTAT     0x0E    /* CAN 状态寄存器                        */
#define REG_CNF1        0x2A    /* 配置寄存器 1 (波特率设定)            */
#define REG_CNF2        0x29    /* 配置寄存器 2                          */
#define REG_CNF3        0x28    /* 配置寄存器 3                          */
#define REG_CANINTE     0x2B    /* 中断使能寄存器                        */
#define REG_CANINTF     0x2C    /* 中断标志寄存器                        */
#define REG_TXB0CTRL    0x30    /* TXB0 控制寄存器                       */
#define REG_TXB0SIDH    0x31    /* TXB0 标准 ID 高字节                   */
#define REG_TXB0SIDL    0x32    /* TXB0 标准 ID 低字节                   */
#define REG_TXB0DLC     0x35    /* TXB0 数据长度码                       */
#define REG_TXB0D0      0x36    /* TXB0 数据字节 0                       */
#define REG_RXB0CTRL    0x60    /* RXB0 控制寄存器                       */
#define REG_RXB0SIDH    0x61    /* RXB0 标准 ID 高字节                   */
#define REG_RXB0SIDL    0x62    /* RXB0 标准 ID 低字节                   */
#define REG_RXB0DLC     0x65    /* RXB0 数据长度码                       */
#define REG_RXB0D0      0x66    /* RXB0 数据字节 0                       */
#define REG_RXB1CTRL    0x70    /* RXB1 控制寄存器                       */

/* ====================================================================== */
/*  SPI 命令                                                              */
/* ====================================================================== */
#define CMD_RESET       0xC0    /* 复位命令                              */
#define CMD_WRITE       0x02    /* 写指令: [0x02] [地址] [数据]         */
#define CMD_READ        0x03    /* 读指令: [0x03] [地址] [0x00]         */
#define CMD_RX_STATUS   0xB0    /* 读 RX 状态: 返回 1 字节              */
                                /*   bit6=1 → RXB0 满                   */
                                /*   bit7=1 → RXB1 满                   */
#define CMD_READ_RXB0   0x90    /* 读 RXB0 缓冲 (含 ID + DLC + 数据)    */
#define CMD_READ_RXB1   0x94    /* 读 RXB1 缓冲 (含 ID + DLC + 数据)    */
#define CMD_BIT_MOD     0x05    /* 位修改: [0x05] [地址] [掩码] [数据]  */
                                /*   值 = (原值 & ~掩码) | (数据 & 掩码) */
#define CMD_LOAD_TXB0   0x40    /* 装载 TXB0 写指令 (从 SIDH 开始)       */
#define CMD_RTS_TXB0    0x81    /* 快速发送 TXB0 (RTS 命令 + bit0)       */

/* ---------- GPIO 中断 ISR ---------- */

/**
 * @brief MCP2515 INT 引脚下降沿中断服务程序
 *
 * ISR 中仅执行最小操作: 通过 Task Notification 通知 main_task 有数据.
 * SPI 操作在任务上下文中完成, 不在 ISR 中执行.
 */
static void IRAM_ATTR mcp2515_isr_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    if (s_notify_task != NULL) {
        vTaskNotifyGiveFromISR(s_notify_task, &woken);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ---------- 内部辅助函数 ---------- */

/**
 * @brief 写 MCP2515 寄存器 (使用标准写指令 0x02)
 *
 * SPI 交互: [0x02] [寄存器地址] [要写入的数据]  (24 bits)
 */
static esp_err_t mcp2515_spi_write(uint8_t reg, uint8_t data)
{
    uint8_t tx[3] = {CMD_WRITE, reg, data};
    return spi_bus_transfer(s_handle, tx, NULL, 24);
}

/**
 * @brief 读 MCP2515 寄存器 (使用标准读指令 0x03)
 *
 * SPI 交互: [0x03] [寄存器地址] [0x00] → 返回第三字节
 */
static esp_err_t mcp2515_spi_read(uint8_t reg, uint8_t *out_data)
{
    uint8_t tx[3] = {CMD_READ, reg, 0x00};
    uint8_t rx[3] = {0};
    esp_err_t ret = spi_bus_transfer(s_handle, tx, rx, 24);
    if (ret == ESP_OK && out_data) {
        *out_data = rx[2];  /* 第三字节为读回值 */
    }
    return ret;
}

/* ---------- 公开 API ---------- */

esp_err_t mcp2515_init(spi_device_handle_t handle, int int_pin)
{
    s_handle = handle;
    s_int_pin = int_pin;

    ESP_LOGI(TAG, "MCP2515 初始化开始 (INT=GPIO%d)", int_pin);

    /* 配置 INT 引脚为输入, 启用内部上拉 */
    gpio_set_direction(int_pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(int_pin, GPIO_PULLUP_ONLY);

    /* 安装 GPIO 中断服务 (仅首次调用有效) */
    if (!s_isr_installed) {
        gpio_install_isr_service(0);                      /* 安装 ISR 服务框架 */
        s_isr_installed = true;
    }
    gpio_set_intr_type(int_pin, GPIO_INTR_NEGEDGE);       /* 下降沿触发        */
    gpio_isr_handler_add(int_pin, mcp2515_isr_handler, NULL);  /* 注册 ISR  */
    ESP_LOGI(TAG, "GPIO%d 下降沿中断已安装", int_pin);

    /* ===== 步骤 1: 复位芯片 ===== */
    {
        uint8_t cmd = CMD_RESET;             /* 0xC0 */
        spi_bus_transfer(s_handle, &cmd, NULL, 8);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "复位完成");

    /* ===== 步骤 2: 设定波特率 ===== */
    mcp2515_spi_write(REG_CNF1, 0x00);      /* 0x02 0x2A 0x00 */
    mcp2515_spi_write(REG_CNF2, 0x90);      /* 0x02 0x29 0x90 */
    mcp2515_spi_write(REG_CNF3, 0x02);      /* 0x02 0x28 0x02 */
    ESP_LOGI(TAG, "波特率配置完成 (CNF1=0x00, CNF2=0x90, CNF3=0x02)");

    /* ===== 步骤 3: 关闭过滤器 (接收所有帧) ===== */
    mcp2515_spi_write(REG_RXB0CTRL, 0x60);  /* 0x02 0x60 0x60: 接收所有帧, 不滤波 */
    mcp2515_spi_write(REG_RXB1CTRL, 0x60);  /* 0x02 0x70 0x60: 同上 */
    ESP_LOGI(TAG, "过滤器已关闭 (接收所有帧)");

    /* ===== 步骤 4: 关闭 RXnBF 引脚中断 ===== */
    mcp2515_spi_write(REG_BFPCTRL, 0x00);   /* 0x02 0x0C 0x00 */
    ESP_LOGI(TAG, "RXnBF 引脚中断已关闭");

    /* ===== 步骤 5: 开启 INT 引脚中断 ===== */
    mcp2515_spi_write(REG_CANINTE, 0x03);   /* 0x02 0x2B 0x03 */
                                            /* bit0=RXB0IE, bit1=RXB1IE */
    ESP_LOGI(TAG, "INT 引脚中断已开启 (RXB0IE + RXB1IE)");

    /* ===== 步骤 6: 清除所有中断标志 ===== */
    mcp2515_spi_write(REG_CANINTF, 0x00);   /* 0x02 0x2C 0x00 */
    ESP_LOGI(TAG, "中断标志已清除");

    /* ===== 步骤 7: 正常工作模式 + 重试机制 ===== */
    bool normal = false;
    for (int retry = 0; retry < 5; retry++) {
        mcp2515_spi_write(REG_CANCTRL, 0x00);   /* 0x02 0x0F 0x00 */
        if (retry > 0) {
            ESP_LOGI(TAG, "重试 %d: 重新配置 CANCTRL=0x00", retry);
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        /* 验证模式切换：检查 OPMOD[2:0] (bit6:4)，全 0 表示正常模式 */
        uint8_t stat = 0;
        mcp2515_spi_read(REG_CANSTAT, &stat);
        uint8_t opmod = (stat >> 4) & 0x07;
        normal = (opmod == 0);

        ESP_LOGI(TAG, "MCP2515 验证 (retry=%d, CANSTAT=0x%02X, OPMOD=%d %s)",
                 retry, stat, opmod, normal ? "Normal ✓" : "异常");

        if (normal) break;

        /* 不满足则复位后重新配置所有寄存器 */
        if (retry < 4) {
            ESP_LOGW(TAG, "OPMOD 异常, 复位重试...");
            uint8_t cmd = CMD_RESET;
            spi_bus_transfer(s_handle, &cmd, NULL, 8);
            vTaskDelay(pdMS_TO_TICKS(10));

            mcp2515_spi_write(REG_CNF1, 0x00);
            mcp2515_spi_write(REG_CNF2, 0x90);
            mcp2515_spi_write(REG_CNF3, 0x02);
            mcp2515_spi_write(REG_RXB0CTRL, 0x60);
            mcp2515_spi_write(REG_RXB1CTRL, 0x60);
            mcp2515_spi_write(REG_BFPCTRL, 0x00);
            mcp2515_spi_write(REG_CANINTE, 0x03);
            mcp2515_spi_write(REG_CANINTF, 0x00);
        }
    }

    return normal ? ESP_OK : ESP_FAIL;
}

void mcp2515_register_task(TaskHandle_t notify_task)
{
    s_notify_task = notify_task;
    ESP_LOGI(TAG, "已注册任务 (handle=0x%p) 接收 MCP2515 中断通知",
             (void *)notify_task);
}

esp_err_t mcp2515_send_frame(const mcp2515_frame_t *frame)
{
    /*
     * 发送流程:
     *   步骤 1: 装载 TXB0 — 发送 0x40 + ID高 + ID低 + 0x00 + 0x00 + DLC + D0~D7
     *           共 14 字节 (112 bits)
     *   步骤 2: 快速发送 — 拉高 CS, 再拉低 CS, 发送单字节 0x81
     *
     *   注: spi_bus_transfer 每次调用会自动拉低/拉高 CS,
     *       因此两次调用自然实现 CS 的完整周期.
     */
    uint8_t dlc = frame->dlc & 0x0F;
    uint8_t sidh = (frame->id >> 3) & 0xFF;       /* ID[10:3] */
    uint8_t sidl = (frame->id & 0x07) << 5;        /* ID[2:0] 左移 5 位 */

    if (frame->is_rtr)      dlc |= 0x40;           /* RTR bit  */
    if (frame->is_extended) dlc |= 0x20;           /* EXT bit  */
    if (frame->is_extended) sidl |= 0x08;           /* EXIDE bit */

    /* 步骤 1: 装载 TXB0 (从 SIDH 开始) */
    uint8_t tx_buf[14] = {
        CMD_LOAD_TXB0,          /* 0x40: 装载 TXB0 写指令, 从 SIDH 开始 */
        sidh,                   /* SIDH: 标准 ID 高字节                   */
        sidl,                   /* SIDL: 标准 ID 低字节                   */
        0x00,                   /* EID8: 扩展 ID 高 (标准帧用 0)          */
        0x00,                   /* EID0: 扩展 ID 低 (标准帧用 0)          */
        dlc,                    /* DLC:  数据长度码                       */
        frame->data[0],         /* D0                                    */
        frame->data[1],         /* D1                                    */
        frame->data[2],         /* D2                                    */
        frame->data[3],         /* D3                                    */
        frame->data[4],         /* D4                                    */
        frame->data[5],         /* D5                                    */
        frame->data[6],         /* D6                                    */
        frame->data[7],         /* D7                                    */
    };
    spi_bus_transfer(s_handle, tx_buf, NULL, 14 * 8);  /* 112 bits */

    /* 步骤 2: 快速发送 TXB0 (CS 自动拉高再拉低) */
    uint8_t rts = CMD_RTS_TXB0;                    /* 0x81 */
    spi_bus_transfer(s_handle, &rts, NULL, 8);

    ESP_LOGI(TAG, "MCP2515 TX: ID=0x%03lX DLC=%d Data=%02X %02X %02X %02X %02X %02X %02X %02X",
             (unsigned long)frame->id, frame->dlc,
             frame->data[0], frame->data[1], frame->data[2], frame->data[3],
             frame->data[4], frame->data[5], frame->data[6], frame->data[7]);

    return ESP_OK;
}

esp_err_t mcp2515_receive_frame(mcp2515_frame_t *frame)
{
    if (!frame) return ESP_ERR_INVALID_ARG;

    /* ========== 步骤 1: 读 RX 状态 (0xB0) ========== */
    uint8_t rx_status_cmd = CMD_RX_STATUS;   /* 0xB0 */
    uint8_t rx_status = 0;
    esp_err_t ret = spi_bus_transfer(s_handle, &rx_status_cmd, &rx_status, 8);
    if (ret != ESP_OK) return ret;

    bool rx0_full = (rx_status & 0x40) != 0; /* bit6=1 → RXB0 有数据 */
    bool rx1_full = (rx_status & 0x80) != 0; /* bit7=1 → RXB1 有数据 */

    if (!rx0_full && !rx1_full) {
        return ESP_ERR_NOT_FOUND;            /* 无数据 */
    }

    /* ========== 步骤 2: 读取数据 (14 字节) ========== */
    uint8_t read_cmd = rx0_full ? CMD_READ_RXB0 : CMD_READ_RXB1;
    /* 发送 1 字节命令 + 13 字节空数据 (0x00), 接收 14 字节 */
    uint8_t tx[14] = {read_cmd, 0,0,0,0,0,0,0,0,0,0,0,0,0};
    uint8_t rx[14] = {0};
    ret = spi_bus_transfer(s_handle, tx, rx, 14 * 8);
    if (ret != ESP_OK) return ret;

    /* 解析标准帧 (只关注前 2 个 ID 字节) */
    /* rx[1] = SIDH, rx[2] = SIDL, rx[5] = DLC, rx[6..13] = D0~D7 */
    frame->id  = ((uint32_t)rx[1] << 3) | (rx[2] >> 5);
    frame->dlc = rx[5] & 0x0F;
    frame->is_rtr  = (rx[5] & 0x40) != 0;
    frame->is_extended = (rx[2] & 0x08) != 0;

    if (frame->dlc > 0 && frame->dlc <= 8 && !frame->is_rtr) {
        memcpy(frame->data, &rx[6], frame->dlc);
    }

    /* ========== 步骤 3: 清除中断标志 (位修改) ========== */
    uint8_t mask = rx0_full ? 0x01 : 0x02;  /* RXB0IF=bit0, RXB1IF=bit1 */
    uint8_t bm[4] = {CMD_BIT_MOD, REG_CANINTF, mask, 0x00};
    /* 0x05 0x2C mask 0x00: 将对应 bit 写 0 以清除 */
    spi_bus_transfer(s_handle, bm, NULL, 32);

    ESP_LOGI(TAG, "收到 CAN 帧 (RXB%c): ID=0x%03lX DLC=%d %s",
             rx0_full ? '0' : '1',
             (unsigned long)frame->id, frame->dlc,
             frame->is_rtr ? "RTR" : "");

    return ESP_OK;
}

bool mcp2515_has_interrupt(void)
{
    if (s_int_pin < 0) return false;
    /* MCP2515 INT 引脚低电平有效 */
    return gpio_get_level(s_int_pin) == 0;
}