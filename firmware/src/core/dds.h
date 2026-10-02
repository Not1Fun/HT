/* @brief 85点整数正弦与170MHz定时器分频，不访问硬件。 */
#ifndef HT_DDS_H
#define HT_DDS_H

#include <stddef.h>
#include <stdint.h>

#define DDS_CYCLE_SAMPLES 85u
#define DDS_BUFFER_SAMPLES (20u * DDS_CYCLE_SAMPLES)
#define DDS_MIDPOINT 2048u
#define DDS_AMPLITUDE_MAX 2047u

/* 成功0，参数无效-1且不改输出；count须为85的倍数且不超过1700。 */
int dds_wave(uint16_t *output, size_t count, uint16_t amplitude);
/* 仅支持2/5/8/10kHz，TIM6输入170MHz、PSC=0。 */
int dds_timer(uint32_t frequency_hz, uint16_t *arr);

#endif
