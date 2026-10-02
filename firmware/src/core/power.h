/* @brief 恒VA输出状态机；所有硬件动作经单一所有者的回调执行。 */
#ifndef HT_POWER_H
#define HT_POWER_H
#include <stdbool.h>
#include <stdint.h>

enum power_state { POWER_IDLE, POWER_MUTING, POWER_BREAK, POWER_TRANS,
    POWER_TAP, POWER_ZERO, POWER_RUNNING, POWER_STOPPING, POWER_RELEASE, POWER_FAULT };
enum power_error { POWER_ERROR_NONE, POWER_ERROR_IO, POWER_ERROR_INTERLOCK,
    POWER_ERROR_SAMPLE, POWER_ERROR_LIMIT, POWER_ERROR_OPEN, POWER_ERROR_SHORT };
struct power_reading {
    uint32_t voltage_mv, current_ma, apparent_mva, sequence;
    int64_t time_ms;
    bool valid;
};
struct power_ops {
    void *ctx;
    int (*mute)(void *ctx);
    int (*select)(void *ctx, uint8_t output);
    int (*start)(void *ctx, uint32_t frequency);
    int (*level)(void *ctx, uint16_t amplitude);
    int64_t (*now)(void *ctx);
    bool (*cancelled)(void *ctx);
};
struct power {
    struct power_ops ops;
    enum power_state state;
    enum power_error error;
    uint8_t range, output;
    uint32_t frequency, target_mva, sequence;
    int64_t deadline, started, feedback, low_since;
    int64_t level_milli;
    uint16_t amplitude;
    bool fault_stopped;
};
int power_init(struct power *power, const struct power_ops *ops);
int power_start(struct power *power, uint8_t range, uint32_t frequency,
                uint32_t target_mva, int64_t now);
void power_stop(struct power *power, int64_t now);
void power_fail(struct power *power, enum power_error error);
void power_poll(struct power *power, bool permitted, const struct power_reading *reading, int64_t now);
#endif
