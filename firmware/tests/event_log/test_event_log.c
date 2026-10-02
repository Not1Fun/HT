/* @brief 验证事件日志的顺序、覆盖边界和失败时状态保持。 */
#include "event_log.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void check_entry(const struct event_log *log, size_t offset,
                        uint32_t seconds, enum event_kind kind, int32_t value)
{
    struct event_entry entry;

    CHECK(event_log_get(log, offset, &entry) == EVENT_LOG_OK);
    CHECK(entry.seconds == seconds && entry.kind == kind && entry.value == value);
}

static void check_get_failure(const struct event_log *log, size_t offset, int expected)
{
    struct event_entry entry;
    unsigned char before[sizeof entry];

    memset(&entry, 0xa5, sizeof entry);
    entry.seconds = 123u;
    entry.kind = EVENT_BOOT;
    entry.value = -456;
    memcpy(before, &entry, sizeof before);
    CHECK(event_log_get(log, offset, &entry) == expected);
    CHECK(memcmp(before, &entry, sizeof before) == 0);
}

static void test_initialization(void)
{
    struct event_log log;

    CHECK(EVENT_LOG_CAPACITY == 32u);
    memset(&log, 0xa5, sizeof log);
    event_log_init(&log);
    CHECK(log.ready && event_log_count(&log) == 0u);
    check_get_failure(&log, 0u, EVENT_LOG_ERR_RANGE);
    for (size_t i = 0; i < EVENT_LOG_CAPACITY + 3u; ++i) {
        CHECK(event_log_add(&log, (uint32_t)i, EVENT_BOOT, (int32_t)i) == EVENT_LOG_OK);
    }
    event_log_init(&log);
    CHECK(log.ready && event_log_count(&log) == 0u);
    check_get_failure(&log, 0u, EVENT_LOG_ERR_RANGE);
    CHECK(event_log_add(&log, 9u, EVENT_IO_ERROR, -7) == EVENT_LOG_OK);
    CHECK(event_log_count(&log) == 1u);
    check_entry(&log, 0u, 9u, EVENT_IO_ERROR, -7);
    event_log_init(&log);
    event_log_init(&log);
    CHECK(event_log_count(&log) == 0u && log.ready);
}

static void test_null_arguments(void)
{
    struct event_log log = {0};
    unsigned char before[sizeof log];

    event_log_init(NULL);
    CHECK(event_log_count(NULL) == 0u);
    CHECK(event_log_add(NULL, 0u, EVENT_BOOT, 0) == EVENT_LOG_ERR_ARG);
    check_get_failure(NULL, 0u, EVENT_LOG_ERR_ARG);
    CHECK(event_log_get(NULL, 0u, NULL) == EVENT_LOG_ERR_ARG);
    event_log_init(&log);
    CHECK(event_log_add(&log, 5u, EVENT_BOOT, 6) == EVENT_LOG_OK);
    memcpy(before, &log, sizeof before);
    CHECK(event_log_get(&log, 0u, NULL) == EVENT_LOG_ERR_ARG);
    CHECK(memcmp(before, &log, sizeof before) == 0);
}

static void test_not_ready(void)
{
    struct event_log log = {0};
    unsigned char before[sizeof log];

    memcpy(before, &log, sizeof before);
    CHECK(event_log_count(&log) == 0u);
    CHECK(event_log_add(&log, 1u, EVENT_BOOT, 2) == EVENT_LOG_ERR_NOT_READY);
    check_get_failure(&log, 0u, EVENT_LOG_ERR_NOT_READY);
    CHECK(memcmp(before, &log, sizeof before) == 0);
    event_log_init(&log);
    CHECK(event_log_add(&log, 1u, EVENT_BOOT, 2) == EVENT_LOG_OK);
    log.ready = false;
    memcpy(before, &log, sizeof before);
    CHECK(event_log_count(&log) == 0u);
    CHECK(event_log_add(&log, 2u, EVENT_RANGE, 3) == EVENT_LOG_ERR_NOT_READY);
    check_get_failure(&log, 0u, EVENT_LOG_ERR_NOT_READY);
    CHECK(memcmp(before, &log, sizeof before) == 0);
}

static void test_bounds(void)
{
    struct event_log log = {0};

    event_log_init(&log);
    check_get_failure(&log, 0u, EVENT_LOG_ERR_RANGE);
    check_get_failure(&log, SIZE_MAX, EVENT_LOG_ERR_RANGE);
    CHECK(event_log_add(&log, 0u, EVENT_BOOT, 0) == EVENT_LOG_OK);
    check_entry(&log, 0u, 0u, EVENT_BOOT, 0);
    check_get_failure(&log, 1u, EVENT_LOG_ERR_RANGE);
    for (size_t i = 1; i < EVENT_LOG_CAPACITY; ++i) {
        CHECK(event_log_add(&log, (uint32_t)i, EVENT_RANGE, (int32_t)i) == EVENT_LOG_OK);
    }
    check_entry(&log, EVENT_LOG_CAPACITY - 1u, 0u, EVENT_BOOT, 0);
    check_get_failure(&log, EVENT_LOG_CAPACITY, EVENT_LOG_ERR_RANGE);
    check_get_failure(&log, SIZE_MAX, EVENT_LOG_ERR_RANGE);
}

static void test_all_kinds(void)
{
    static const enum event_kind kinds[] = {
        EVENT_BOOT, EVENT_DISPLAY_ONLINE, EVENT_DISPLAY_OFFLINE,
        EVENT_RANGE, EVENT_FREQUENCY, EVENT_BATTERY_OK, EVENT_BATTERY_ALARM,
        EVENT_OC_ACTIVE, EVENT_OC_CLEAR, EVENT_OV_ACTIVE, EVENT_OV_CLEAR,
        EVENT_TEMP_READY, EVENT_TEMP_INVALID, EVENT_TEMP_INIT_FAILED, EVENT_IO_ERROR,
        EVENT_DDS_START, EVENT_DDS_STOP, EVENT_DDS_FAILED
    };
    const size_t count = sizeof kinds / sizeof kinds[0];
    struct event_log log = {0};

    CHECK(EVENT_NONE == 0 && count == (size_t)EVENT_COUNT - 1u);
    event_log_init(&log);
    for (size_t i = 0; i < count; ++i) {
        CHECK(kinds[i] == (enum event_kind)(i + 1u));
        CHECK(event_log_add(&log, (uint32_t)i, kinds[i], (int32_t)i) == EVENT_LOG_OK);
        CHECK(event_log_count(&log) == i + 1u);
    }
    for (size_t i = 0; i < count; ++i) {
        size_t original = count - 1u - i;

        check_entry(&log, i, (uint32_t)original, kinds[original], (int32_t)original);
    }
}

static void test_value_limits(void)
{
    static const int32_t values[] = {INT32_MIN, INT32_MAX, -1, 0, 1};
    const size_t count = sizeof values / sizeof values[0];
    struct event_log log = {0};

    event_log_init(&log);
    for (size_t i = 0; i < count; ++i) {
        CHECK(event_log_add(&log, UINT32_MAX, EVENT_IO_ERROR, values[i]) == EVENT_LOG_OK);
    }
    for (size_t i = 0; i < count; ++i) {
        check_entry(&log, i, UINT32_MAX, EVENT_IO_ERROR, values[count - 1u - i]);
    }
}

static void test_capacity_and_wrap(void)
{
    struct {
        uint32_t before;
        struct event_log log;
        uint32_t after;
    } guarded = {.before = 0x12345678u, .after = 0xabcdef01u};

    event_log_init(&guarded.log);
    for (size_t i = 0; i < EVENT_LOG_CAPACITY * 5u + 7u; ++i) {
        size_t expected_count = i < EVENT_LOG_CAPACITY ? i + 1u : EVENT_LOG_CAPACITY;

        CHECK(event_log_add(&guarded.log, (uint32_t)i, EVENT_RANGE,
                            (int32_t)i - 50) == EVENT_LOG_OK);
        CHECK(event_log_count(&guarded.log) == expected_count);
        for (size_t offset = 0; offset < expected_count; ++offset) {
            size_t original = i - offset;

            check_entry(&guarded.log, offset, (uint32_t)original, EVENT_RANGE,
                        (int32_t)original - 50);
        }
        check_get_failure(&guarded.log, expected_count, EVENT_LOG_ERR_RANGE);
        CHECK(guarded.before == 0x12345678u && guarded.after == 0xabcdef01u);
    }
}

static void test_timestamp_order(void)
{
    static const uint32_t seconds[] = {
        UINT32_MAX - 1u, UINT32_MAX, 0u, 1u, 0u, 77u, UINT32_MAX, 2u
    };
    const size_t count = sizeof seconds / sizeof seconds[0];
    struct event_log log = {0};

    event_log_init(&log);
    for (size_t i = 0; i < count; ++i) {
        CHECK(event_log_add(&log, seconds[i], EVENT_FREQUENCY, (int32_t)i) == EVENT_LOG_OK);
    }
    for (size_t i = 0; i < count; ++i) {
        size_t original = count - 1u - i;

        check_entry(&log, i, seconds[original], EVENT_FREQUENCY, (int32_t)original);
    }
}

static void test_independent_instances(void)
{
    struct event_log first = {0};
    struct event_log second = {0};
    unsigned char before[sizeof second];

    event_log_init(&first);
    event_log_init(&second);
    for (size_t i = 0; i < 40u; ++i) {
        CHECK(event_log_add(&first, (uint32_t)i, EVENT_OC_ACTIVE, (int32_t)i) == EVENT_LOG_OK);
        if (i % 3u == 0u) {
            CHECK(event_log_add(&second, (uint32_t)i, EVENT_OV_CLEAR,
                                -(int32_t)i) == EVENT_LOG_OK);
        }
    }
    CHECK(event_log_count(&first) == EVENT_LOG_CAPACITY);
    CHECK(event_log_count(&second) == 14u);
    for (size_t i = 0; i < 14u; ++i) {
        uint32_t seconds = 39u - (uint32_t)i * 3u;

        check_entry(&second, i, seconds, EVENT_OV_CLEAR, -(int32_t)seconds);
    }
    memcpy(before, &second, sizeof before);
    event_log_init(&first);
    CHECK(event_log_add(&first, 99u, EVENT_BOOT, 100) == EVENT_LOG_OK);
    CHECK(memcmp(before, &second, sizeof before) == 0);
    check_entry(&first, 0u, 99u, EVENT_BOOT, 100);
}

static void test_failure_unchanged(void)
{
    static const enum event_kind invalid[] = {
        EVENT_NONE, EVENT_COUNT, (enum event_kind)-1,
        (enum event_kind)(EVENT_COUNT + 1), (enum event_kind)INT_MAX
    };
    struct event_log log = {0};
    unsigned char before[sizeof log];

    event_log_init(&log);
    for (size_t i = 0; i < EVENT_LOG_CAPACITY + 7u; ++i) {
        CHECK(event_log_add(&log, (uint32_t)i, EVENT_BOOT, (int32_t)i) == EVENT_LOG_OK);
    }
    memcpy(before, &log, sizeof before);
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        CHECK(event_log_add(&log, UINT32_MAX, invalid[i], INT32_MIN) == EVENT_LOG_ERR_ARG);
        CHECK(memcmp(before, &log, sizeof before) == 0);
    }
    check_get_failure(&log, event_log_count(&log), EVENT_LOG_ERR_RANGE);
    check_get_failure(&log, SIZE_MAX, EVENT_LOG_ERR_RANGE);
    CHECK(event_log_get(&log, 0u, NULL) == EVENT_LOG_ERR_ARG);
    CHECK(memcmp(before, &log, sizeof before) == 0);
    check_entry(&log, 0u, EVENT_LOG_CAPACITY + 6u, EVENT_BOOT,
                (int32_t)EVENT_LOG_CAPACITY + 6);
    CHECK(memcmp(before, &log, sizeof before) == 0);
}

static void test_duplicate_events(void)
{
    struct event_log log = {0};

    event_log_init(&log);
    for (size_t i = 0; i < EVENT_LOG_CAPACITY + 2u; ++i) {
        size_t expected_count = i < EVENT_LOG_CAPACITY ? i + 1u : EVENT_LOG_CAPACITY;

        CHECK(event_log_add(&log, 0u, EVENT_BATTERY_ALARM, -1) == EVENT_LOG_OK);
        CHECK(event_log_count(&log) == expected_count);
    }
    for (size_t i = 0; i < EVENT_LOG_CAPACITY; ++i) {
        check_entry(&log, i, 0u, EVENT_BATTERY_ALARM, -1);
    }
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"initialization", test_initialization},
        {"null_arguments", test_null_arguments},
        {"not_ready", test_not_ready},
        {"bounds", test_bounds},
        {"all_kinds", test_all_kinds},
        {"value_limits", test_value_limits},
        {"capacity_and_wrap", test_capacity_and_wrap},
        {"timestamp_order", test_timestamp_order},
        {"independent_instances", test_independent_instances},
        {"failure_unchanged", test_failure_unchanged},
        {"duplicate_events", test_duplicate_events}
    };

    if (argc == 2) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            if (strcmp(argv[1], cases[i].name) == 0) {
                cases[i].run();
                return EXIT_SUCCESS;
            }
        }
    }
    fprintf(stderr, "usage: event_log_tests <case>\n");
    return EXIT_FAILURE;
}
