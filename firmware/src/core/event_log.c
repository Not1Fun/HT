/* @brief 固定容量事件环缓冲，按从新到旧的偏移复制记录。 */
#include "event_log.h"

void event_log_init(struct event_log *log)
{
    if (log != NULL) {
        *log = (struct event_log){.ready = true};
    }
}

int event_log_add(struct event_log *log, uint32_t seconds, enum event_kind kind, int32_t value)
{
    if (log == NULL || kind <= EVENT_NONE || kind >= EVENT_COUNT) {
        return EVENT_LOG_ERR_ARG;
    }
    if (!log->ready) {
        return EVENT_LOG_ERR_NOT_READY;
    }
    log->entries[log->head] = (struct event_entry){seconds, kind, value};
    log->head = (log->head + 1) % EVENT_LOG_CAPACITY;
    if (log->count < EVENT_LOG_CAPACITY) {
        ++log->count;
    }
    return EVENT_LOG_OK;
}

int event_log_get(const struct event_log *log, size_t newest_offset, struct event_entry *entry)
{
    size_t index;

    if (log == NULL || entry == NULL) {
        return EVENT_LOG_ERR_ARG;
    }
    if (!log->ready) {
        return EVENT_LOG_ERR_NOT_READY;
    }
    if (newest_offset >= log->count) {
        return EVENT_LOG_ERR_RANGE;
    }
    index = (log->head + EVENT_LOG_CAPACITY - 1 - newest_offset) % EVENT_LOG_CAPACITY;
    *entry = log->entries[index];
    return EVENT_LOG_OK;
}

size_t event_log_count(const struct event_log *log)
{
    return log != NULL && log->ready ? log->count : 0;
}
