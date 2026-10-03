/* @brief 正式输出的ADC12与DDS协调者，温度维护不与独立温度驱动并用。 */
#ifndef HT_SIGNAL_IO_H
#define HT_SIGNAL_IO_H
#include "core/power.h"
#include "platform/temperature_io.h"
struct signal_snapshot {
    struct power_reading reading;
    struct temperature_snapshot temperature;
    int64_t temperature_ms;
};
/* 初始化后持续测量；待机仅启用TIM6采样触发。 */
int signal_io_init(void);
int signal_io_start(uint32_t frequency);
/* 停止波形并恢复待机采样；故障锁存时不恢复。 */
int signal_io_stop(void);
int signal_io_poll(struct signal_snapshot *snapshot);
/* 可从ISR调用，不执行I2C或等待，锁存至复位。 */
void signal_io_fault(void);
bool signal_io_failed(void);
#endif
