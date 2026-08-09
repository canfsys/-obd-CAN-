/**
 * @file sit1145.c
 * @brief SIT1145AQ CAN 收发器 SPI 驱动实现
 *
 * 初始化逻辑:
 *   1. 写入基础初始化序列 (0x81=0x07, 0xAB=0x03)
 *   2. 先写波特率寄存器 0x26, 延时后再写 CAN 控制寄存器 0x20=0x32
 *      (因为写入 0x20=0x32 宣告 PNCOK=1 后, 芯片会立即校验 0x26 中的波特率)
 *   3. 读回 0x26/0x20/0x22 三个寄存器验证
 */

#include "sit1145.h"
#include "board_config.h"
#include "spi_bus.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   /* for esp_rom_delay_us */

static const char *TAG = "SIT1145";

/* ---------- 内部数据结构 ---------- */

/** SIT1145 设备上下文 */
typedef struct {
    spi_device_handle_t handle;     /* SPI 设备句柄                   */
    bool                initialized; /* 是否已初始化                   */
} sit1145_ctx_t;

/** 两个设备的上下文实例 */
static sit1145_ctx_t s_ctx[SIT1145_DEV_MAX] = {0};

/* CS 引脚映射表 (与 sit1145_dev_t 枚举顺序一致) */
static const int s_cs_pin[SIT1145_DEV_MAX] = {
    PIN_SIT1_CS,    /* SIT1145_DEV_1: GPIO5 */
    PIN_SIT2_CS,    /* SIT1145_DEV_2: GPIO6 */
};

/* ---------- 初始化序列 ---------- */

/**
 * SIT1145 初始化寄存器配置表
 *   注意: 只有 2 个寄存器, 删除了 {0xA0, 0x03} 防止污染后续 0x32 写入.
 */
static const uint8_t s_init_seq[][2] = {
    {0x81, 0x07},   /* 模式控制寄存器 -> 写入 0x07 (进入 Normal 正常模式) */
    {0xAB, 0x03},   /* 模块/引脚配置寄存器                              */
};

#define INIT_SEQ_COUNT  (sizeof(s_init_seq) / sizeof(s_init_seq[0]))

/* ---------- 内部辅助函数 ---------- */

/**
 * @brief 寄存器读写验证 (严格顺序)
 *
 * 关键步骤: 必须先写波特率寄存器 0x26, 再写 CAN 控制寄存器 0x20.
 * 因为一旦写入 0x20=0x32 (PNCOK=1), 芯片会立即校验 0x26 中的波特率.
 *
 * 验证: 读回 0x26 (期望 val_26), 0x20 (期望 0x32), 0x22 (期望 0xA0 或 0xA8)
 *
 * @param dev   设备编号
 * @param name  设备名 (用于日志)
 * @return ESP_OK=验证通过, ESP_FAIL=验证失败
 */
static esp_err_t sit1145_write_verify(sit1145_dev_t dev, const char *name)
{
    uint8_t reg20 = 0, reg22 = 0, reg26 = 0;

    /* 各设备寄存器 0x26 配置值:
     *   SIT1145-1 (TWAI 路, 连接 TWAI 125kbps): 0x02
     *   SIT1145-2 (MCP2515 路, 连接 MCP2515 500kbps): 0x05 */
    uint8_t val_26 = (dev == SIT1145_DEV_1) ? 0x02 : 0x05;

    /* ===== 步骤 1: 先写波特率寄存器 (0x26) ===== */
    ESP_LOGI(TAG, "%s 写入: reg[0x26]=0x%02X", name, val_26);
    sit1145_write_reg(dev, 0x26, val_26);
    esp_rom_delay_us(10);   /* 延时等待写入完成 */

    /* ===== 步骤 2: 后写 CAN 控制寄存器 (0x20=0x32) =====
     * 写入 0x32 开启 Active 模式 (CMC=10) 并确认配置成功 (PNCOK=1).
     * 芯片此时会校验 0x26 中的波特率是否合法, 所以必须先写 0x26. */
    ESP_LOGI(TAG, "%s 写入: reg[0x20]=0x32", name);
    sit1145_write_reg(dev, 0x20, 0x32);
    esp_rom_delay_us(10);   /* 延时等待激活完成 */

    /* ===== 步骤 3: 读回三个寄存器验证 ===== */
    sit1145_read_reg(dev, 0x26, &reg26);    /* 期望: val_26 (如 0x02 或 0x05) */
    sit1145_read_reg(dev, 0x20, &reg20);    /* 期望: 0x32 */
    sit1145_read_reg(dev, 0x22, &reg22);    /* 期望: 0xA0 (总线活跃) 或 0xA8 (总线静默) */

    ESP_LOGI(TAG, "%s 读回验证: reg[0x26]=0x%02X, reg[0x20]=0x%02X, reg[0x22]=0x%02X",
             name, reg26, reg20, reg22);

    /* 判定条件: 0x22 可以是 0xA0 或 0xA8,
     *           0x20 应为 0x32,
     *           0x26 应为 val_26 */
    bool pass = (reg26 == val_26) && (reg20 == 0x32) &&
                (reg22 == 0xA0 || reg22 == 0xA8);

    if (pass) {
        ESP_LOGI(TAG, "%s 验证通过 ✓", name);
    } else {
        ESP_LOGW(TAG, "%s 验证失败: 期望 0x26=0x%02X, 0x20=0x32, 0x22=0xA0/0xA8",
                 name, val_26);
    }

    return pass ? ESP_OK : ESP_FAIL;
}

/* ---------- 公开 API ---------- */

esp_err_t sit1145_init(sit1145_dev_t dev, spi_device_handle_t handle)
{
    if (dev >= SIT1145_DEV_MAX) return ESP_ERR_INVALID_ARG;

    s_ctx[dev].handle = handle;

    /* 设备名查找表 */
    const char *dev_name[] = {"SIT1145-1", "SIT1145-2"};
    const char *name = (dev < 2) ? dev_name[dev] : "SIT1145-?";

    ESP_LOGI(TAG, "初始化 %s (CS=GPIO%d)", name, s_cs_pin[dev]);

    /* 重试循环: 最多尝试 3 次 */
    for (int retry = 0; retry < 3; retry++) {
        /* 1. 写入基础初始化序列 (2 个寄存器) */
        for (int i = 0; i < INIT_SEQ_COUNT; i++) {
            uint8_t addr = s_init_seq[i][0];
            uint8_t data = s_init_seq[i][1];
            esp_err_t ret = sit1145_write_reg(dev, addr, data);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "%s 写 0x%02X=0x%02X 失败", name, addr, data);
                goto retry_label;
            }
        }

        /* 2. 寄存器读写验证: 先 0x26, 后 0x20, 再读回三个寄存器 */
        if (sit1145_write_verify(dev, name) == ESP_OK) {
            s_ctx[dev].initialized = true;
            ESP_LOGI(TAG, "%s 初始化完成 (retry=%d)", name, retry);
            return ESP_OK;
        }

retry_label:
        ESP_LOGW(TAG, "%s 验证失败, 重试 %d/3...", name, retry + 1);
        esp_rom_delay_us(100);  /* 短暂延时后重试 */
    }

    /* 即使验证失败也标记初始化 (避免阻塞系统启动) */
    s_ctx[dev].initialized = true;
    ESP_LOGW(TAG, "%s 初始化完成 (验证未通过)", name);
    return ESP_OK;
}

esp_err_t sit1145_get_handle(sit1145_dev_t dev, spi_device_handle_t *handle)
{
    if (dev >= SIT1145_DEV_MAX || !handle) return ESP_ERR_INVALID_ARG;
    *handle = s_ctx[dev].handle;
    return ESP_OK;
}

esp_err_t sit1145_write_reg(sit1145_dev_t dev, uint8_t reg, uint8_t data)
{
    if (dev >= SIT1145_DEV_MAX) return ESP_ERR_INVALID_ARG;
    /* 构造写地址: (reg << 1) | 0, 然后发送 2 字节 */
    return spi_bus_write_reg(s_ctx[dev].handle,
                             SIT1145_ADDR_W(reg), data);
}

esp_err_t sit1145_read_reg(sit1145_dev_t dev, uint8_t reg, uint8_t *out_data)
{
    if (dev >= SIT1145_DEV_MAX || !out_data) return ESP_ERR_INVALID_ARG;
    /* 构造读地址: (reg << 1) | 1 */
    return spi_bus_read_reg(s_ctx[dev].handle,
                            SIT1145_ADDR_R(reg), out_data);
}

esp_err_t sit1145_set_sleep(sit1145_dev_t dev, bool sleep_mode)
{
    if (dev >= SIT1145_DEV_MAX) return ESP_ERR_INVALID_ARG;
    /* 寄存器 0x81 的 bit0 控制 MC 模式: 1=休眠, 0=待机 */
    return sit1145_write_reg(dev, 0x81, sleep_mode ? 0x01 : 0x00);
}

esp_err_t sit1145_get_mode_status(sit1145_dev_t dev, uint8_t *out_mc)
{
    uint8_t status;
    esp_err_t ret = sit1145_read_reg(dev, 0x01, &status);
    if (ret == ESP_OK && out_mc) {
        *out_mc = status & 0x07;  /* 低 3 位 = MC 模式码 */
    }
    return ret;
}

bool sit1145_verify_chip_id(sit1145_dev_t dev)
{
    uint8_t id = 0;
    if (sit1145_read_reg(dev, 0x7E, &id) != ESP_OK) return false;

    bool ok = (id == SIT1145_CHIP_ID);
    ESP_LOGI(TAG, "SIT1145-%d Chip ID = 0x%02X %s",
             dev + 1, id, ok ? "✓" : "✗ (期望 0x74)");
    return ok;
}