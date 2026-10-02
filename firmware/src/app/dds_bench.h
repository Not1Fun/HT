/* @brief 有界、默认停止的DAC台架命令服务；不控制继电器或CC目标。 */
#ifndef HT_DDS_BENCH_H
#define HT_DDS_BENCH_H

#include <stdbool.h>
#include <stdint.h>

struct dds_bench_snapshot {
	bool ready;
	bool running;
	bool permitted;
	bool faulted;
	int last_error;
	uint32_t frequency_hz; /* running时才表示实际启用的DAC频率。 */
	uint16_t requested_mvpp;
	uint16_t amplitude_code;
	uint32_t elapsed_seconds;
	uint32_t duration_seconds;
};

/* init/poll/stop仅由主线程调用；init不启动输出，也不能用于清故障。 */
int dds_bench_init(void);
/* 每轮服务调用，执行时重查许可；许可失效取消排队请求且停波。 */
int dds_bench_poll(bool permitted);
int dds_bench_stop(void);
/* shell/屏幕共用异步请求；成功只代表入队，输出以snapshot.running为准。 */
int dds_bench_request_start(uint32_t frequency_hz, uint16_t mvpp, uint16_t seconds);
/* 可重复请求；优先取消待启动请求，主线程下一次poll执行停止。 */
int dds_bench_request_stop(void);
/* 复制已发布快照，可从shell线程调用，不访问硬件。 */
int dds_bench_snapshot(struct dds_bench_snapshot *snapshot);

#endif
