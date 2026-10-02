/* @brief 三路NTC的有界慢采样，独占ADC12资源，不启动激励。 */
#ifndef HT_TEMPERATURE_IO_H
#define HT_TEMPERATURE_IO_H

#include "core/ntc.h"

#define TEMPERATURE_CHANNEL_COUNT 3u

struct temperature_sample {
	uint16_t raw;
	int16_t decicelsius;
	enum ntc_status status;
};

struct temperature_snapshot {
	/* 固定顺序：CN6/PC3/IN9、CN7/PA1/IN2、CN8/PA0/IN1。 */
	struct temperature_sample samples[TEMPERATURE_CHANNEL_COUNT];
};

/* 单线程串行使用；只在AVDD及探头接线确认后显式初始化。
 * ADC1/2设备树必须禁用，ADC12时钟已被其他模块开启则拒绝。
 * 当前联调独占ADC12，后续同步采集驱动不能与本模块并用。
 */
int temperature_io_init(void);
/* 平台错误返回负errno、全部标为无效并停止后续采样，需显式重新init。
 * 返回0时仍须逐路检查status；探头异常不会中止其他两路。
 * 调用者约每200ms读取，并负责失期显示；本模块不提供缓存/后台任务。
 */
int temperature_io_read(struct temperature_snapshot *snapshot);
/* 有界停止；不关闭由board_io管理的VREF，也不恢复任何输出。 */
void temperature_io_stop(void);

#endif
