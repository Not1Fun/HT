/* @brief 核对频率恒等式、全部幅度码和正弦误差，测试不访问硬件。 */
#include "core/dds.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t data[DDS_BUFFER_SAMPLES + 2u];
static const double pi = 3.14159265358979323846;

static void frequencies(void)
{
	const uint32_t frequency[] = {2000u, 5000u, 8000u, 10000u};
	const uint16_t expected[] = {999u, 399u, 249u, 199u};
	for (unsigned int i = 0; i < 4; ++i) {
		uint16_t arr = 0;
		assert(dds_timer(frequency[i], &arr) == 0);
		assert(arr == expected[i]);
		assert((uint32_t)(arr + 1u) * DDS_CYCLE_SAMPLES * frequency[i] == 170000000u);
		assert(DDS_CYCLE_SAMPLES * frequency[i] <= 1000000u);
	}
}

static void invalid_frequency(void)
{
	const uint32_t values[] = {0, 1, 1999, 2001, 4999, 8001, 10001, UINT32_MAX};
	uint16_t arr = 1234;
	for (unsigned int i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
		assert(dds_timer(values[i], &arr) == -1);
		assert(arr == 1234);
	}
	assert(dds_timer(2000, NULL) == -1);
}

static void amplitude_sweep(void)
{
	for (uint16_t amp = 0; amp <= DDS_AMPLITUDE_MAX; ++amp) {
		uint32_t sum = 0;
		assert(dds_wave(data, DDS_CYCLE_SAMPLES, amp) == 0);
		for (unsigned int i = 0; i < DDS_CYCLE_SAMPLES; ++i) {
			assert(data[i] >= DDS_MIDPOINT - amp);
			assert(data[i] <= DDS_MIDPOINT + amp);
			sum += data[i];
			if (i != 0) {
				assert(data[i] + data[DDS_CYCLE_SAMPLES - i] == 2u * DDS_MIDPOINT);
			}
		}
		assert(sum == DDS_MIDPOINT * DDS_CYCLE_SAMPLES);
	}
}

static void sine_error(void)
{
	for (unsigned int amp = 1; amp <= DDS_AMPLITUDE_MAX; ++amp) {
		assert(dds_wave(data, DDS_CYCLE_SAMPLES, (uint16_t)amp) == 0);
		for (unsigned int i = 0; i < DDS_CYCLE_SAMPLES; ++i) {
			double ideal = DDS_MIDPOINT + amp * sin(2.0 * pi * i / DDS_CYCLE_SAMPLES);
			assert(fabs(data[i] - ideal) < 0.532);
		}
	}
}

static void periodicity(void)
{
	assert(dds_wave(data, DDS_BUFFER_SAMPLES, DDS_AMPLITUDE_MAX) == 0);
	for (unsigned int i = DDS_CYCLE_SAMPLES; i < DDS_BUFFER_SAMPLES; ++i) {
		assert(data[i] == data[i % DDS_CYCLE_SAMPLES]);
	}
	assert(data[0] == DDS_MIDPOINT);
	assert(data[DDS_BUFFER_SAMPLES - 1u] < DDS_MIDPOINT);
	/* 包括DMA环绕边界，最大相邻步幅不得超过峰值斜率+量化。 */
	for (unsigned int i = 0; i < DDS_BUFFER_SAMPLES; ++i) {
		int step = (int)data[i] - data[(i + 1u) % DDS_BUFFER_SAMPLES];
		assert(step >= -153 && step <= 153);
	}
}

static void zero_and_bench(void)
{
	assert(dds_wave(data, DDS_BUFFER_SAMPLES, 0) == 0);
	for (unsigned int i = 0; i < DDS_BUFFER_SAMPLES; ++i) {
		assert(data[i] == DDS_MIDPOINT);
	}
	assert(dds_wave(data, DDS_CYCLE_SAMPLES, 70) == 0);
	uint16_t low = 4095, high = 0;
	for (unsigned int i = 0; i < DDS_CYCLE_SAMPLES; ++i) {
		if (data[i] < low) { low = data[i]; }
		if (data[i] > high) { high = data[i]; }
	}
	assert(low == 1978 && high == 2118);
	assert((high - low) * 2900u <= 100u * 4095u);
}

static void invalid_buffer(void)
{
	const size_t lengths[] = {0, 1, 84, 86, 1699, 1701, SIZE_MAX};
	memset(data, 0xa5, sizeof(data));
	for (unsigned int i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
		assert(dds_wave(data, lengths[i], 1) == -1);
	}
	assert(dds_wave(data, DDS_CYCLE_SAMPLES, 2048) == -1);
	assert(dds_wave(data, DDS_CYCLE_SAMPLES, UINT16_MAX) == -1);
	assert(dds_wave(NULL, DDS_CYCLE_SAMPLES, 1) == -1);
	for (unsigned int i = 0; i < DDS_BUFFER_SAMPLES + 2u; ++i) {
		assert(data[i] == 0xa5a5);
	}
}

static void boundaries(void)
{
	for (unsigned int cycles = 1; cycles <= 20; ++cycles) {
		memset(data, 0xa5, sizeof(data));
		size_t count = cycles * DDS_CYCLE_SAMPLES;
		assert(dds_wave(&data[1], count, 70) == 0);
		assert(data[0] == 0xa5a5 && data[count + 1u] == 0xa5a5);
	}
}

int main(int argc, char **argv)
{
	assert(argc == 2);
#define RUN(name) if (strcmp(argv[1], #name) == 0) { name(); puts("DDS test passed"); return 0; }
	RUN(frequencies)
	RUN(invalid_frequency)
	RUN(amplitude_sweep)
	RUN(sine_error)
	RUN(periodicity)
	RUN(zero_and_bench)
	RUN(invalid_buffer)
	RUN(boundaries)
	assert(!"unknown test");
	return 1;
}
