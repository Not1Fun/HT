/* @brief 恒VA输出状态机；所有硬件动作经单一所有者的回调执行。 */
#ifndef HT_POWER_H
#define HT_POWER_H
#include <stdbool.h>
#include <stdint.h>
#define POWER_RANGE_AUTO UINT8_MAX

enum power_state { POWER_IDLE, POWER_MUTING, POWER_BREAK, POWER_TRANS,
    POWER_TAP, POWER_ZERO, POWER_PROBING, POWER_RUNNING, POWER_STOPPING, POWER_RELEASE, POWER_FAULT };
enum power_error { POWER_ERROR_NONE, POWER_ERROR_IO, POWER_ERROR_INTERLOCK,
    POWER_ERROR_SAMPLE, POWER_ERROR_LIMIT, POWER_ERROR_OPEN, POWER_ERROR_SHORT, POWER_ERROR_MATCH,
    POWER_ERROR_TARGET, POWER_ERROR_FEEDBACK, POWER_ERROR_COUNT };
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
    bool automatic, matching;
    uint8_t candidate, stable, changes, rematches, overload;
    uint16_t ramp_base;
    uint32_t noise_mv, noise_ma;
    uint32_t load_mohm;
    int64_t limit_since;
    int64_t session_ms, match_deadline, probe_after, resident_since;
};
int power_init(struct power *power, const struct power_ops *ops);
/* 标称增益/匝比给出前馈峰值码；闭环仍以实测VA为准。 */
uint16_t power_feedforward(uint8_t range, uint32_t target_mva, uint32_t load_mohm);
uint16_t power_drive_limit(void);
/* AUTO先探测负载再选挡；0..6为手动挡。频率由用户选择，控制器不改频率。 */
int power_start(struct power *power, uint8_t range, uint32_t frequency,
                uint32_t target_mva, int64_t now);
void power_stop(struct power *power, int64_t now);
void power_fail(struct power *power, enum power_error error);
void power_poll(struct power *power, bool permitted, const struct power_reading *reading, int64_t now);
#endif
