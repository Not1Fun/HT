/* @brief 由分压求NTC阻值，按用户141点Rnorm表线性插值。 */
#include "ntc.h"

#include <stddef.h>

/* reference/ntc/B3950-10K-RT-table.pdf，第1页Rnorm列。
 * -20..120 C，每1 C一点，单位0.1 ohm；保留扫描表的全部四位kOhm小数。
 */
static const uint32_t resistance[] = {
	/* -20..-11 */
	874288, 827877, 784386, 743586, 705269, 669248, 635354, 603432, 573341, 544954,
	/* -10..-1 */
	518154, 492833, 468895, 446248, 424810, 404505, 385264, 367020, 349714, 333292,
	/* 0..9 */
	317700, 302534, 288159, 274531, 261608, 249350, 237720, 226684, 216209, 206263,
	/* 10..19 */
	196819, 187848, 179326, 171227, 163530, 156212, 149254, 142637, 136342, 130353,
	/* 20..29 */
	124654, 119229, 114064, 109146, 104462, 100000, 95749, 91697, 87836, 84155,
	/* 30..39 */
	80644, 77297, 74103, 71057, 68149, 65375, 62726, 60197, 57782, 55475,
	/* 40..49 */
	53271, 51165, 49153, 47229, 45390, 43632, 41950, 40341, 38802, 37330,
	/* 50..59 */
	35920, 34571, 33279, 32042, 30857, 29722, 28634, 27592, 26593, 25636,
	/* 60..69 */
	24718, 23837, 22993, 22183, 21405, 20659, 19943, 19256, 18596, 17962,
	/* 70..79 */
	17353, 16768, 16206, 15666, 15147, 14648, 14168, 13707, 13263, 12836,
	/* 80..89 */
	12425, 12030, 11649, 11283, 10930, 10590, 10267, 9955, 9654, 9363,
	/* 90..99 */
	9083, 8812, 8550, 8297, 8052, 7816, 7587, 7366, 7152, 6945,
	/* 100..109 */
	6744, 6558, 6376, 6199, 6026, 5858, 5694, 5535, 5380, 5229,
	/* 110..119 */
	5083, 4941, 4803, 4669, 4539, 4412, 4290, 4171, 4055, 3944,
	3835 /* 120 */
};

_Static_assert(sizeof(resistance) / sizeof(resistance[0]) == 141, "NTC table size");

enum ntc_status ntc_from_resistance(uint32_t resistance_deciohm, int16_t *decicelsius)
{
	if (decicelsius == NULL) {
		return NTC_INVALID_ARGUMENT;
	}
	if (resistance_deciohm > resistance[0] || resistance_deciohm < resistance[140]) {
		return NTC_OUT_OF_RANGE;
	}
	for (size_t i = 1; i < sizeof(resistance) / sizeof(resistance[0]); ++i) {
		if (resistance_deciohm >= resistance[i]) {
			uint32_t span = resistance[i - 1] - resistance[i];
			uint64_t offset = (uint64_t)(resistance[i - 1] - resistance_deciohm) * 10u;

			*decicelsius = (int16_t)(-200 + (int32_t)(i - 1) * 10 +
					       (int32_t)((offset + span / 2u) / span));
			return NTC_OK;
		}
	}
	return NTC_OUT_OF_RANGE;
}

enum ntc_status ntc_convert(uint16_t raw, const struct ntc_config *config,
			   int16_t *decicelsius)
{
	if (config == NULL || decicelsius == NULL || raw > NTC_ADC_MAX ||
	    config->pullup_ohm == 0 || config->pullup_ohm > 1000000u ||
	    config->pullup_mv == 0 || config->reference_mv == 0) {
		return NTC_INVALID_ARGUMENT;
	}
	if (raw == 0) {
		return NTC_SHORT;
	}
	if (raw == NTC_ADC_MAX) {
		return NTC_SATURATED;
	}
	uint64_t node = (uint64_t)raw * config->reference_mv;
	uint64_t supply = (uint64_t)NTC_ADC_MAX * config->pullup_mv;

	if (node >= supply) {
		return NTC_SATURATED;
	}
	uint64_t denominator = supply - node;
	uint64_t value = ((uint64_t)config->pullup_ohm * 10u * node + denominator / 2u) /
			 denominator;

	if (value > UINT32_MAX) {
		return NTC_OUT_OF_RANGE;
	}
	return ntc_from_resistance((uint32_t)value, decicelsius);
}
