/**
 * @file can_buffer.h
 * @brief CAN 数据双缓冲区管理器 (PSRAM)
 *
 * 在 PSRAM 中分配两个 64KB 缓冲区, 一个用于写入, 另一个用于 SD 卡写入.
 * 缓冲区满后自动切换并通知 SD 写入任务.
 *
 * 每个缓冲区存储 CSV 格式的 CAN 数据行, 每行格式:
 *   timestamp_ms,can_id,dlc,data,extended,rtr
 *
 * 例如: 0,512,8,01 02 03 04 05 06 07 08,0,0
 */

#ifndef CAN_BUFFER_H
#define CAN_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 默认每个缓冲区大小 (64KB, 在 PSRAM 中分配) */
#define CAN_BUF_SIZE       (65536)

/** PSRAM 不可用时降级缓冲区大小 (16KB, 在内部 RAM 中分配) */
#define CAN_BUF_SIZE_FALLBACK (16384)

/** CSV 行缓冲最大长度 (时间戳+ID+DLC+数据Hex+标志 约 64 字节) */
#define CSV_LINE_MAX   96

/**
 * @brief 缓冲区上下文 (每个 CAN 源独立)
 */
typedef struct {
    char   *buf[2];               /*!< 两个缓冲区                         */
    uint32_t buf_size;             /*!< 实际每个缓冲区大小 (PSRAM OK=64KB, 降级=16KB) */
    int     active_idx;            /*!< 当前写入的缓冲区索引 (0 或 1)      */
    uint32_t write_pos;            /*!< 当前缓冲区写入偏移                 */
    uint32_t frame_count;          /*!< 当前缓冲区已写入帧数               */
    uint32_t full_size;            /*!< 满缓冲区数据大小 (切换时记录)       */
    SemaphoreHandle_t lock;        /*!< 缓冲区互斥锁                      */
    SemaphoreHandle_t full_sem;    /*!< 缓冲区满信号量 (通知 SD 任务)      */
    SemaphoreHandle_t shared_sem;  /*!< 共享计数信号量 (替代轮询)          */
    uint64_t pseudo_time_ms;       /*!< 伪时间戳计数器 (ms)               */
    char    *name;                 /*!< 名称 (用于日志)                    */
} can_buffer_ctx_t;

/**
 * @brief 初始化双缓冲区上下文
 *
 * 在 PSRAM 中分配两个 64KB 缓冲区, 创建互斥锁和信号量.
 *
 * @param ctx  缓冲区上下文指针
 * @param name 名称 (如 "sit1145" 或 "mcp2515")
 * @return esp_err_t
 */
esp_err_t can_buffer_init(can_buffer_ctx_t *ctx, const char *name);

/**
 * @brief 写入一行 CSV 数据到当前活跃缓冲区
 *
 * 内部会锁定互斥锁, 写入到活跃缓冲区.
 * 如果写满 (write_pos >= 65536) 则自动切换缓冲区并释放信号量.
 *
 * @param ctx         缓冲区上下文
 * @param can_id      CAN ID
 * @param dlc         数据长度
 * @param data        CAN 数据 (8 字节)
 * @param is_extended 是否扩展帧
 * @param is_rtr      是否远程帧
 */
void can_buffer_write_csv(can_buffer_ctx_t *ctx, uint32_t can_id, uint8_t dlc,
                          const uint8_t *data, bool is_extended, bool is_rtr);

/**
 * @brief 获取已满缓冲区的指针和大小 (用于 SD 写入)
 *
 * @param ctx       缓冲区上下文
 * @param out_buf   输出缓冲区指针
 * @param out_size  输出数据大小
 * @return true=有满缓冲区, false=无
 */
bool can_buffer_get_full(can_buffer_ctx_t *ctx, char **out_buf, uint32_t *out_size);

/**
 * @brief 释放已满缓冲区 (SD 写入完成后调用)
 *
 * @param ctx 缓冲区上下文
 */
void can_buffer_release_full(can_buffer_ctx_t *ctx);

/**
 * @brief 注册共享计数信号量 (替代轮询, 任务永久阻塞等待)
 *
 * 当缓冲区满时, 内部会调用 xSemaphoreGive(shared_sem).
 * SD 任务只需等待这一个信号量, 无需超时轮询.
 *
 * @param ctx        缓冲区上下文
 * @param shared_sem 共享计数信号量句柄
 */
void can_buffer_set_shared_sem(can_buffer_ctx_t *ctx, SemaphoreHandle_t shared_sem);

/**
 * @brief 获取缓冲区名称
 *
 * @param ctx 缓冲区上下文
 * @return const char*
 */
const char *can_buffer_get_name(can_buffer_ctx_t *ctx);

/**
 * @brief 获取实际缓冲区大小
 *
 * @param ctx 缓冲区上下文
 * @return uint32_t 字节数
 */
uint32_t can_buffer_get_size(can_buffer_ctx_t *ctx);

/**
 * @brief 释放缓冲区内存
 *
 * @param ctx 缓冲区上下文
 */
void can_buffer_deinit(can_buffer_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CAN_BUFFER_H */