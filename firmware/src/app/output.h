/* @brief 屏幕只提交整机启停请求；后台负责继电器、测量与恒VA控制。 */
#ifndef HT_OUTPUT_H
#define HT_OUTPUT_H
#include "platform/signal_io.h"
#include "core/output_reason.h"
struct output_snapshot {
    bool ready, available, active, running, switching, matching, fault;
    int error;
    uint8_t range;
    uint32_t frequency, elapsed_seconds, target_mva;
    uint32_t blocked;
    bool clearing, clear_needed;
    uint32_t clear_count;
    int clear_result;
#if defined(CONFIG_HT_OUTPUT_BENCH)
    struct { bool busy, wave, trial, dac_available; uint8_t coils; uint32_t seconds, blocked; } debug;
#endif
    struct signal_snapshot signal;
};
int output_init(void);
void output_inputs(uint8_t raw, bool connected, bool io_ok);
int output_start(uint8_t range, uint32_t frequency, uint32_t target_mva);
/* 输出停止后显式清故障；异步结果见快照，成功也不恢复输出。 */
int output_clear(void);
#if defined(CONFIG_HT_OUTPUT_BENCH)
int output_debug(uint8_t relay, uint32_t frequency, uint16_t millivolts_pp, bool wave);
#endif
void output_stop(void);
void output_snapshot(struct output_snapshot *value);
void output_shutdown(void);
#endif
