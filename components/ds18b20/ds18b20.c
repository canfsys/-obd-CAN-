/**
 * @file ds18b20.c
 * @brief DS18B20 温度传感器驱动实现 (单总线协议)
 *
 * 通过 GPIO 模拟单总线时序, 实现 DS18B20 的复位、读写位/字节操作.
 * 温度读取流程: 复位 → 跳过 ROM(0xCC) → 启动转换(0x44) → 等待
 *              → 复位 → 跳过 ROM(0xCC) → 读暂存器(0xBE) → 读取温度值
 */

#include "ds18b20.h"
#include "board_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static const char *TAG = "DS18B20";
static int s_gpio = -1;                     /* 数据引脚号 */

/* ---------- 底层 GPIO 操作宏 ---------- */

/** 微秒延时 */
static inline void delay_us(int us) { esp_rom_delay_us(us); }

/** 设置为开漏输出模式 */
static inline void set_output(void) { gpio_set_direction(s_gpio, GPIO_MODE_OUTPUT_OD); }

/** 设置为输入模式 (高阻, 靠上拉保持高电平) */
static inline void set_input(void)  { gpio_set_direction(s_gpio, GPIO_MODE_INPUT); }

/** 输出低电平 */
static inline void write_low(void)  { gpio_set_level(s_gpio, 0); }

/** 输出高电平 (开漏模式下需切为输入或写1) */
static inline void write_high(void) { gpio_set_level(s_gpio, 1); }

/** 读取引脚电平 */
static inline int read_pin(void)    { return gpio_get_level(s_gpio); }

/* ---------- 单总线协议函数 ---------- */

/**
 * @brief 复位脉冲 + 检测存在脉冲
 *
 * 主机拉低 480μs 释放, 等待 DS18B20 拉低 60~240μs 作为应答.
 *
 * @return 1=检测到 DS18B20, 0=无器件
 */
static int ds18b20_reset(void)
{
    set_output(); write_low();      /* 拉低总线 480μs           */
    delay_us(500);
    set_input(); write_high();      /* 释放总线, 上拉至高       */
    delay_us(70);                    /* 等待器件应答             */
    int presence = (read_pin() == 0); /* 检测低电平 (存在脉冲)   */
    delay_us(430);                   /* 等待复位时序结束         */
    if (!presence) ESP_LOGW(TAG, "未检测到 DS18B20");
    return presence;
}

/**
 * @brief 写一个位
 *
 * 拉低 2μs 后, 若写入 1 则释放总线, 若写入 0 则继续拉低.
 * 总时序约 65μs.
 */
static void write_bit(int bit)
{
    set_output(); write_low(); delay_us(2);
    if (bit) write_high();          /* 写 1: 释放总线           */
    delay_us(60);                    /* 保持电平 60μs           */
    set_input(); write_high(); delay_us(2); /* 恢复高阻          */
}

/**
 * @brief 读一个位
 *
 * 拉低 2μs 释放后, 在 15μs 内采样总线电平.
 * 总时序约 65μs.
 */
static int read_bit(void)
{
    int bit;
    set_output(); write_low(); delay_us(2);
    set_input(); write_high(); delay_us(8);
    bit = read_pin();               /* 采样                        */
    delay_us(55);                    /* 等待时序完成                */
    return bit;
}

/**
 * @brief 写一个字节 (LSB first)
 */
static void write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) { write_bit(byte & 0x01); byte >>= 1; }
}

/**
 * @brief 读一个字节 (LSB first)
 */
static uint8_t read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) { byte >>= 1; if (read_bit()) byte |= 0x80; }
    return byte;
}

/* ---------- 公开 API ---------- */

esp_err_t ds18b20_init(int gpio_pin)
{
    s_gpio = gpio_pin;
    gpio_reset_pin(gpio_pin);
    gpio_set_pull_mode(gpio_pin, GPIO_PULLUP_ONLY);  /* 启用内部上拉 */
    ESP_LOGI(TAG, "DS18B20 初始化完成 (GPIO=%d)", gpio_pin);
    return ESP_OK;
}

bool ds18b20_is_present(void)
{
    if (s_gpio < 0) return false;
    return ds18b20_reset() != 0;
}

esp_err_t ds18b20_read_temp(int32_t *out_temp_c)
{
    if (!out_temp_c) return ESP_ERR_INVALID_ARG;
    if (s_gpio < 0) return ESP_ERR_INVALID_STATE;

    /* 第 1 步: 复位, 启动温度转换 */
    if (!ds18b20_reset()) return ESP_ERR_INVALID_RESPONSE;
    write_byte(0xCC);               /* 跳过 ROM (单器件时使用) */
    write_byte(0x44);               /* 启动温度转换             */
    delay_us(100);                   /* 等待转换启动             */

    /* 第 2 步: 复位, 读取暂存器 */
    if (!ds18b20_reset()) return ESP_ERR_INVALID_RESPONSE;
    write_byte(0xCC);               /* 跳过 ROM                 */
    write_byte(0xBE);               /* 读暂存器 (Scratchpad)    */

    /* 读取温度值: 第 1 字节 = LSB, 第 2 字节 = MSB */
    uint8_t lsb = read_byte();
    uint8_t msb = read_byte();

    /* 12-bit 温度值: MSB << 8 | LSB, 分辨率 0.0625°C
     * 温度 (m°C) = raw * 1000 / 16 */
    int16_t raw = (int16_t)((msb << 8) | lsb);
    *out_temp_c = (int32_t)raw * 1000 / 16;

    ESP_LOGD(TAG, "温度: raw=0x%04X, %" PRId32 " m°C", raw, *out_temp_c);
    return ESP_OK;
}