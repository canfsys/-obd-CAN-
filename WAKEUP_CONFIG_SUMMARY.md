# ESP32-S3 唤醒配置问题 — 待排查

## 硬件信息

| 项目 | 值 |
|------|-----|
| 芯片 | ESP32-S3-WROOM-1-N8R2 |
| 唤醒引脚 | GPIO1 (无外部上下拉电阻) |
| 唤醒信号 | 外部手动加 3.3V 脉冲电压 (秒级) |

## 目标行为

1. **正常模式** → 2 分钟无 CAN 报文 → **待机模式 (light sleep)**
2. **待机模式** → GPIO1 高电平脉冲 → 应唤醒回到正常模式
3. **待机模式** → 5 分钟定时器到期 → **休眠模式 (deep sleep)**
4. **休眠模式** → GPIO1 高电平脉冲 → 应唤醒重启芯片

## 当前现象

- 进入 light sleep / deep sleep 成功（终端有打印）
- GPIO1 加 3.3V 高电平脉冲后 **没有任何反应**（终端无打印）
- 说明 GPIO1 没有触发唤醒

## 代码片段

所有唤醒配置代码均在 `components/power_manager/power_manager.c` 中。

### ① 待机模式 (light sleep) — power_manager_enter_standby()

```c
/* ===== 进入 light sleep 前的唤醒配置 ===== */

/* GPIO1: 输入 + 内部下拉, EXT1 高电平唤醒 */
gpio_set_direction(GPIO_NUM_1, GPIO_MODE_INPUT);
gpio_set_pull_mode(GPIO_NUM_1, GPIO_PULLDOWN_ONLY);
esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_1, ESP_EXT1_WAKEUP_ANY_HIGH);

/* 5 分钟 RTC 定时器唤醒 */
esp_sleep_enable_timer_wakeup(SLEEP_TIMER_TIMEOUT_US);  // 5*60*1000000 = 300000000 us

/* 进入 light sleep */
ESP_LOGI(TAG, "进入 light sleep, 5 分钟定时器已设");
fflush(stdout);
esp_light_sleep_start();

/* ===== 唤醒后处理 ===== */
esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
ESP_LOGI(TAG, "从 light sleep 唤醒, 原因: %d", cause);

// 恢复 GPIO38/39 电源控制
// ...

if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    // 5 分钟无唤醒 → 进入休眠模式 (deep sleep)
    power_manager_enter_sleep();
}

// GPIO 唤醒 → 回到正常模式
return ESP_OK;  // 返回后重新初始化外设
```

### ② 休眠模式 (deep sleep) — power_manager_enter_sleep()

```c
/* ===== 进入 deep sleep 前的唤醒配置 ===== */

/* GPIO1: 输入 + 内部下拉, EXT1 高电平唤醒 */
gpio_set_direction(GPIO_NUM_1, GPIO_MODE_INPUT);
gpio_set_pull_mode(GPIO_NUM_1, GPIO_PULLDOWN_ONLY);
esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_1, ESP_EXT1_WAKEUP_ANY_HIGH);

/* 进入 deep sleep (不返回) */
esp_deep_sleep_start();
```

### ③ 待机前关闭的 GPIO

```c
/* 关闭 SD 卡电源 */
gpio_set_level(PIN_SD_PWR_EN, 0);   // GPIO38
/* 关闭 ADC 测量 */
gpio_set_level(PIN_BAT_MEAS_EN, 0); // GPIO39
```

## 已排查的方向

| 方向 | 结果 |
|------|------|
| `GPIO_INTR_HIGH_LEVEL` + `esp_sleep_enable_gpio_wakeup()` 旧版 IO MUX 路径 | 无效 |
| `ESP_GPIO_WAKEUP_GPIO_HIGH` + `esp_sleep_enable_gpio_wakeup(GPIO, level)` 新版 API | 编译不通过 (API 不存在) |
| 旧版 + IO MUX 混合 EXT1 (同时调用 `esp_sleep_enable_gpio_wakeup()` 和 `esp_sleep_enable_ext1_wakeup()`) | **互相冲突** — 全部唤醒失效 |
| 仅 EXT1 (`esp_sleep_enable_ext1_wakeup`) + RTC 定时器 | **当前代码, 仍然无效** |
| GPIO42 加入 EXT1 | 失败 — GPIO42 不是 RTC IO |

## 核心疑问

1. ESP32-S3 上，对于 **GPIO1 (RTC IO)**，light sleep 和 deep sleep 的 EXT1 唤醒最低配置步骤是否就是：
   ```
   gpio_set_direction(GPIO_NUM_1, GPIO_MODE_INPUT);
   gpio_set_pull_mode(GPIO_NUM_1, GPIO_PULLDOWN_ONLY);
   esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_1, ESP_EXT1_WAKEUP_ANY_HIGH);
   esp_light_sleep_start(); // 或 esp_deep_sleep_start()
   ```
   还有没有缺失的步骤？

2. `esp_sleep_enable_timer_wakeup()` 和 `esp_sleep_enable_ext1_wakeup()` 是否可以同时使用？

3. 是否需要额外调用 `esp_sleep_pd_config()` 之类的函数来保持唤醒电路供电？

4. GPIO1 无外部上下拉电阻，仅靠内部 45kΩ 下拉，是否足够可靠？是否必须在 GPIO1 加外部 10kΩ 下拉？