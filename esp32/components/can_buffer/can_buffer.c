/**
 * @file can_buffer.c
 * @brief CAN 数据双缓冲区管理器实现 (PSRAM 优先, 降级到内部 RAM)
 *
 * 优先使用 heap_caps_malloc(, MALLOC_CAP_SPIRAM) 在 PSRAM 中分配缓冲区.
 * 如果 PSRAM 不可用 (esp_psram_get_size() 返回 0),
 * 则降级到内部 RAM, 缓冲区大小从 64KB 缩小到 16KB.
 *
 * 写入 CSV 格式数据, 满后切换缓冲区并通过信号量通知 SD 任务.
 */

#include "can_buffer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "CAN_BUF";

esp_err_t can_buffer_init(can_buffer_ctx_t *ctx, const char *name)
{
    if (!ctx || !name) return ESP_ERR_INVALID_ARG;

    memset(ctx, 0, sizeof(can_buffer_ctx_t));
    ctx->name = strdup(name);

    /* 检查 PSRAM 是否可用 */
    size_t psram_size = esp_psram_get_size();
    bool use_psram = (psram_size > 0);
    uint32_t caps = MALLOC_CAP_SPIRAM;
    uint32_t buf_size = CAN_BUF_SIZE;

    if (!use_psram) {
        ESP_LOGW(TAG, "%s: PSRAM 不可用 (大小=%zu), 降级到内部 RAM, 缓冲区缩小至 %u",
                 name, psram_size, CAN_BUF_SIZE_FALLBACK);
        caps = MALLOC_CAP_INTERNAL;
        buf_size = CAN_BUF_SIZE_FALLBACK;
    } else {
        ESP_LOGI(TAG, "%s: PSRAM 总大小: %zu bytes, 分配 2×%u 字节缓冲区",
                 name, psram_size, (unsigned)buf_size);
    }

    ctx->buf_size = buf_size;

    /* 分配两个缓冲区 */
    for (int i = 0; i < 2; i++) {
        ctx->buf[i] = (char *)heap_caps_malloc(buf_size, caps);
        if (!ctx->buf[i]) {
            ESP_LOGE(TAG, "%s: %s 分配 buf[%d] 失败 (大小=%u, 内存类型=%s)!",
                     name, use_psram ? "PSRAM" : "内部 RAM", i,
                     (unsigned)buf_size, use_psram ? "SPIRAM" : "INTERNAL");
            can_buffer_deinit(ctx);
            return ESP_ERR_NO_MEM;
        }
        memset(ctx->buf[i], 0, buf_size);
        ESP_LOGD(TAG, "%s: buf[%d] @ 0x%p (%u bytes)",
                 name, i, (void *)ctx->buf[i], (unsigned)buf_size);
    }

    ctx->active_idx = 0;
    ctx->write_pos = 0;
    ctx->frame_count = 0;
    ctx->full_size = 0;
    ctx->pseudo_time_ms = 0;
    ctx->shared_sem = NULL;

    /* 创建互斥锁 (保护写入) */
    ctx->lock = xSemaphoreCreateMutex();
    if (!ctx->lock) {
        ESP_LOGE(TAG, "%s: 创建互斥锁失败", name);
        can_buffer_deinit(ctx);
        return ESP_ERR_NO_MEM;
    }

    /* 创建二进制信号量 (满缓冲区通知 SD 任务) */
    ctx->full_sem = xSemaphoreCreateBinary();
    if (!ctx->full_sem) {
        ESP_LOGE(TAG, "%s: 创建信号量失败", name);
        can_buffer_deinit(ctx);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "%s: 双缓冲区初始化完成 (%s, 2×%u bytes)",
             name, use_psram ? "PSRAM" : "内部 RAM", (unsigned)buf_size);
    return ESP_OK;
}

void can_buffer_write_csv(can_buffer_ctx_t *ctx, uint32_t can_id, uint8_t dlc,
                          const uint8_t *data, bool is_extended, bool is_rtr)
{
    if (!ctx || !ctx->buf[0] || !ctx->buf[1]) return;

    char line[CSV_LINE_MAX];
    int len;

    /* 伪造时间戳 (后续替换为真实时间) */
    uint64_t ts = ctx->pseudo_time_ms++;

    /* 构造 CSV 行: timestamp_ms,can_id,dlc,d0~d7,extended,rtr
     * 每个数据字节单独一列, 不满8字节的空列留空 */
    if (data && dlc > 0 && dlc <= 8) {
        len = snprintf(line, sizeof(line),
                       "%llu,%lu,%d,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%d,%d\r\n",
                       (unsigned long long)ts,
                       (unsigned long)can_id, dlc,
                       (dlc >= 1) ? data[0] : 0,
                       (dlc >= 2) ? data[1] : 0,
                       (dlc >= 3) ? data[2] : 0,
                       (dlc >= 4) ? data[3] : 0,
                       (dlc >= 5) ? data[4] : 0,
                       (dlc >= 6) ? data[5] : 0,
                       (dlc >= 7) ? data[6] : 0,
                       (dlc >= 8) ? data[7] : 0,
                       is_extended ? 1 : 0, is_rtr ? 1 : 0);
    } else {
        /* 远程帧 (无数据, d0~d7 全部为 0) */
        len = snprintf(line, sizeof(line),
                       "%llu,%lu,%d,0,0,0,0,0,0,0,0,%d,%d\r\n",
                       (unsigned long long)ts,
                       (unsigned long)can_id, dlc,
                       is_extended ? 1 : 0, is_rtr ? 1 : 0);
    }

    if (len <= 0 || len >= CSV_LINE_MAX) return;

    /* 锁定互斥锁后写入 */
    if (xSemaphoreTake(ctx->lock, portMAX_DELAY) == pdTRUE) {
        int active = ctx->active_idx;

        /* 检查是否有足够空间, 不够则切换到另一个缓冲区 */
        if (ctx->write_pos + len >= ctx->buf_size) {
            /* 当前缓冲区写满, 记录数据大小供 SD 读取 */
            ctx->full_size = ctx->write_pos;

            ESP_LOGI(TAG, "%s: 缓冲区 %d 写满 (%lu frames/%u bytes), 切换到缓冲区 %d",
                     ctx->name, active, (unsigned long)ctx->frame_count,
                     (unsigned)ctx->write_pos, 1 - active);

            /* 切换缓冲区 */
            ctx->active_idx = 1 - active;
            ctx->write_pos = 0;
            ctx->frame_count = 0;

            /* 通知 SD 任务 (专用信号量 + 共享信号量) */
            xSemaphoreGive(ctx->full_sem);
            if (ctx->shared_sem != NULL) {
                xSemaphoreGive(ctx->shared_sem);
            }

            active = ctx->active_idx;
        }

        /* 写入 CSV 行 (切换后写入新缓冲区, 防止数据丢失) */
        memcpy(ctx->buf[active] + ctx->write_pos, line, len);
        ctx->write_pos += len;
        ctx->frame_count++;

        xSemaphoreGive(ctx->lock);
    }
}

bool can_buffer_get_full(can_buffer_ctx_t *ctx, char **out_buf, uint32_t *out_size)
{
    if (!ctx || !out_buf || !out_size) return false;

    /* 获取非活跃缓冲区 */
    int full_idx = 1 - ctx->active_idx;

    if (ctx->full_size == 0) return false;  /* 无数据 */

    *out_buf = ctx->buf[full_idx];
    *out_size = ctx->full_size;

    ESP_LOGI(TAG, "%s: 获取满缓冲区 %d, size=%u bytes (%lu frames)",
             ctx->name, full_idx, (unsigned)ctx->full_size,
             (unsigned long)(ctx->full_size / CSV_LINE_MAX));
    return true;
}

void can_buffer_release_full(can_buffer_ctx_t *ctx)
{
    if (!ctx) return;

    int full_idx = 1 - ctx->active_idx;
    ctx->full_size = 0;  /* 先清零再清内存, 防止并发问题 */
    memset(ctx->buf[full_idx], 0, ctx->buf_size);

    ESP_LOGD(TAG, "%s: 释放缓冲区 %d", ctx->name, full_idx);
}

void can_buffer_set_shared_sem(can_buffer_ctx_t *ctx, SemaphoreHandle_t shared_sem)
{
    if (ctx) ctx->shared_sem = shared_sem;
}

const char *can_buffer_get_name(can_buffer_ctx_t *ctx)
{
    return ctx ? ctx->name : "NULL";
}

uint32_t can_buffer_get_size(can_buffer_ctx_t *ctx)
{
    return ctx ? ctx->buf_size : 0;
}

void can_buffer_deinit(can_buffer_ctx_t *ctx)
{
    if (!ctx) return;

    for (int i = 0; i < 2; i++) {
        if (ctx->buf[i]) {
            heap_caps_free(ctx->buf[i]);
            ctx->buf[i] = NULL;
        }
    }

    if (ctx->lock) {
        vSemaphoreDelete(ctx->lock);
        ctx->lock = NULL;
    }

    if (ctx->full_sem) {
        vSemaphoreDelete(ctx->full_sem);
        ctx->full_sem = NULL;
    }

    if (ctx->name) {
        free((void *)ctx->name);
        ctx->name = NULL;
    }

    ESP_LOGI(TAG, "缓冲区已释放");
}