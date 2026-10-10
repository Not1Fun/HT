/* @brief PA4 DAC台架波形的独占配置、启停和故障状态。 */
#ifndef HT_DDS_IO_H
#define HT_DDS_IO_H

#include <stdbool.h>
#include <stdint.h>

enum dds_fault {
	DDS_FAULT_NONE = 0,
	DDS_FAULT_DMA,
	DDS_FAULT_DAC,
	DDS_FAULT_RESOURCE,
	DDS_FAULT_STOP,
	DDS_FAULT_EXTERNAL
};

struct dds_snapshot {
	bool ready;
	bool running;
	uint32_t frequency_hz; /* 已配置值；只有running时代表启用的波形。 */
	uint16_t amplitude;   /* DAC峰值码，不是mV，不是功率闭环目标。 */
	enum dds_fault fault;
};

/* 以下接口仅在线程调用，返回0或负errno。init不会启用DAC。 */
int dds_io_init(void);
int dds_io_configure(uint32_t frequency_hz, uint16_t amplitude);
int dds_io_start(void);
#if defined(CONFIG_HT_OUTPUT)
/* ADC已armed后调用：只启用TIM6采样触发，保持DAC停机状态；切回输出需stop/configure。 */
int dds_io_sample_start(uint32_t frequency_hz);
/* 只更新DMA已释放半区；完成两半区后才发布实际幅度。 */
int dds_io_set_amplitude(uint16_t amplitude);
#endif
/* 正常停机保留已启用DAC的2048中点；返回成功不等于模拟输出已稳定。 */
int dds_io_stop(void);
/* 输出已停止后核对资源并清除旧故障；不会恢复波形或旧幅度。 */
int dds_io_recover(void);
int dds_io_check(void);
int dds_io_snapshot(struct dds_snapshot *snapshot);
/* 普通ISR/线程均可调用；纯寄存器停止并锁存故障。 */
void dds_io_fault_stop(void);

#endif
