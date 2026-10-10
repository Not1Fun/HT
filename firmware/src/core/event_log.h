/* @brief 保存本次上电的有限事件记录，不依赖硬件、时钟或日志后端。 */
#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EVENT_LOG_CAPACITY 32u

enum event_boot_mode {
    EVENT_BOOT_NORMAL = 0,
    EVENT_BOOT_BENCH
};

enum event_kind {
    EVENT_NONE = 0,
    EVENT_BOOT,
    EVENT_DISPLAY_ONLINE,
    EVENT_DISPLAY_OFFLINE,
    EVENT_RANGE,
    EVENT_FREQUENCY,
    EVENT_BATTERY_OK,
    EVENT_BATTERY_ALARM,
    EVENT_OC_ACTIVE,
    EVENT_OC_CLEAR,
    EVENT_OV_ACTIVE,
    EVENT_OV_CLEAR,
    EVENT_TEMP_READY,
    EVENT_TEMP_INVALID,
    EVENT_TEMP_INIT_FAILED,
    EVENT_IO_ERROR,
    EVENT_DDS_START,
    EVENT_DDS_STOP,
    EVENT_DDS_FAILED,
    EVENT_FAULT_CLEAR,
    EVENT_FAULT_CLEAR_FAILED,
    EVENT_COUNT
};

enum event_log_result {
    EVENT_LOG_OK = 0,
    EVENT_LOG_ERR_ARG = -1,
    EVENT_LOG_ERR_NOT_READY = -2,
    EVENT_LOG_ERR_RANGE = -3
};

struct event_entry {
    uint32_t seconds;
    enum event_kind kind;
    int32_t value;
};

/* 单调用者串行维护；应用层负责边沿去重，禁止在 ISR 并发写入。 */
struct event_log {
    struct event_entry entries[EVENT_LOG_CAPACITY];
    size_t head;
    size_t count;
    bool ready;
};

/* 清空并就绪；NULL 无动作。 */
void event_log_init(struct event_log *log);
/* 满时覆盖最旧；seconds 原样保存，按追加顺序而非时间数值排序。
 * value: BOOT 为 event_boot_mode，RANGE 为 ohm，FREQUENCY 为 Hz，TEMP_READY/INVALID 为通道 1..3；
 * INIT_FAILED/IO_ERROR 为错误码，其余由调用者约定。
 */
int event_log_add(struct event_log *log, uint32_t seconds, enum event_kind kind, int32_t value);
/* newest_offset=0 取最新；失败不修改 entry，NULL 参数/未就绪/越界返回负错误。 */
int event_log_get(const struct event_log *log, size_t newest_offset, struct event_entry *entry);
/* NULL 或未就绪返回 0。 */
size_t event_log_count(const struct event_log *log);

#endif
