/* @brief 台架继电器与固定幅度试波；由输出线程独占执行。 */
#ifndef HT_BENCH_H
#define HT_BENCH_H
#include "core/power.h"

enum bench_state { BENCH_IDLE, BENCH_MUTE, BENCH_BREAK, BENCH_TRANS,
    BENCH_TAP, BENCH_ZERO, BENCH_ON, BENCH_STOP, BENCH_RELEASE, BENCH_FAULT };
struct bench {
    struct power_ops ops;
    enum bench_state state;
    enum power_error error;
    uint8_t relay, output;
    uint16_t amplitude;
    uint32_t frequency;
    bool wave, fault_stopped;
    int64_t deadline, expires;
};
/* relay: 0全释放、1仅K1、2..8对应K2..K8抽头（自动配合K1）。 */
uint8_t bench_mask(uint8_t relay);
int bench_init(struct bench *bench, const struct power_ops *ops);
int bench_start(struct bench *bench, uint8_t relay, uint32_t frequency,
                uint16_t millivolts_pp, bool wave, int64_t now);
void bench_stop(struct bench *bench, int64_t now);
void bench_fail(struct bench *bench, enum power_error error);
void bench_poll(struct bench *bench, bool permitted, const struct power_reading *reading, int64_t now);
#endif
