/* @brief 固定Q15正弦表生成有界12位波形和四个精确频率挡位。 */
#include "core/dds.h"

/* round(32767 * sin(2*pi*k/85))；运行时不使用浮点。 */
static const int16_t sine[DDS_CYCLE_SAMPLES] = {
	0, 2420, 4827, 7207, 9548, 11837, 14061, 16208, 18267, 20226,
	22075, 23803, 25401, 26860, 28173, 29332, 30330, 31163, 31826, 32315,
	32627, 32761, 32717, 32493, 32092, 31516, 30768, 29851, 28772, 27536,
	26149, 24619, 22955, 21165, 19260, 17250, 15145, 12958, 10700, 8383,
	6021, 3626, 1211, -1211, -3626, -6021, -8383, -10700, -12958, -15145,
	-17250, -19260, -21165, -22955, -24619, -26149, -27536, -28772, -29851, -30768,
	-31516, -32092, -32493, -32717, -32761, -32627, -32315, -31826, -31163, -30330,
	-29332, -28173, -26860, -25401, -23803, -22075, -20226, -18267, -16208, -14061,
	-11837, -9548, -7207, -4827, -2420
};

int dds_wave(uint16_t *output, size_t count, uint16_t amplitude)
{
	if (output == NULL || count == 0 || count > DDS_BUFFER_SAMPLES ||
	    count % DDS_CYCLE_SAMPLES != 0 || amplitude > DDS_AMPLITUDE_MAX) {
		return -1;
	}
	for (size_t i = 0; i < count; ++i) {
		int32_t product = (int32_t)amplitude * sine[i % DDS_CYCLE_SAMPLES];
		int32_t delta = product >= 0 ? (product + 16383) / 32767 :
			-((-product + 16383) / 32767);

		output[i] = (uint16_t)((int32_t)DDS_MIDPOINT + delta);
	}
	return 0;
}

int dds_timer(uint32_t frequency_hz, uint16_t *arr)
{
	if (arr == NULL || (frequency_hz != 2000u && frequency_hz != 5000u &&
			   frequency_hz != 8000u && frequency_hz != 10000u)) {
		return -1;
	}
	*arr = (uint16_t)(170000000u / (DDS_CYCLE_SAMPLES * frequency_hz) - 1u);
	return 0;
}
