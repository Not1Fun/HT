/* @brief 用户表锚点、分压参考独立性与全ADC码域的主机验证。 */
#include "core/ntc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

static const struct ntc_config nominal = {22000, 3300, 2900};

static void test_anchors(void)
{
	/* 独立选取PDF Rnorm锚点，覆盖表格三段、零点及最高温端。 */
	static const struct { uint32_t r; int16_t t; } anchors[] = {
		{874288, -200}, {669248, -150}, {518154, -100}, {404505, -50},
		{317700, 0}, {196819, 100}, {124654, 200}, {100000, 250},
		{80644, 300}, {53271, 400}, {35920, 500}, {24718, 600},
		{17353, 700}, {12425, 800}, {12030, 810}, {11649, 820},
		{9083, 900}, {6744, 1000}, {5083, 1100}, {3835, 1200}
	};
	for (size_t i = 0; i < sizeof(anchors) / sizeof(anchors[0]); ++i) {
		int16_t t = -999;

		CHECK(ntc_from_resistance(anchors[i].r, &t) == NTC_OK);
		CHECK(t == anchors[i].t);
	}
}

static void test_interpolation(void)
{
	int16_t t;

	CHECK(ntc_from_resistance(341503, &t) == NTC_OK && t == -15);
	CHECK(ntc_from_resistance(97874, &t) == NTC_OK && t == 255);
	CHECK(ntc_from_resistance(3890, &t) == NTC_OK && t == 1195);
	CHECK(ntc_from_resistance(317699, &t) == NTC_OK && t == 0);
}

static void test_limits(void)
{
	int16_t t = 1234;
	const uint32_t invalid[] = {0, 3834, 874289, UINT32_MAX};

	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
		CHECK(ntc_from_resistance(invalid[i], &t) == NTC_OUT_OF_RANGE);
		CHECK(t == 1234);
	}
	CHECK(ntc_from_resistance(874288, &t) == NTC_OK && t == -200);
	CHECK(ntc_from_resistance(3835, &t) == NTC_OK && t == 1200);
}

static void test_nominal_adc(void)
{
	/* 25C节点=1.03125V，参考2.9V对应约1456码；50C约654码。 */
	int16_t t;

	CHECK(ntc_convert(1456, &nominal, &t) == NTC_OK && t == 250);
	CHECK(ntc_convert(654, &nominal, &t) == NTC_OK && t == 500);
	CHECK(ntc_convert(2753, &nominal, &t) == NTC_OK && t == 0);
}

static void test_reference_config(void)
{
	struct ntc_config config = nominal;
	int16_t t;

	/* 同一25C探头换成3.3V参考后码值约1280；参考和上拉必须分开。 */
	config.reference_mv = 3300;
	CHECK(ntc_convert(1280, &config, &t) == NTC_OK && t == 250);
	CHECK(ntc_convert(1280, &nominal, &t) == NTC_OK && t > 280);
	config = nominal;
	config.pullup_ohm = 10000;
	CHECK(ntc_convert(2330, &config, &t) == NTC_OK && t == 250);
	/* 上拉电压也可独立注入，不固定假设3300mV。 */
	config = nominal;
	config.pullup_mv = 2900;
	CHECK(ntc_convert(1280, &config, &t) == NTC_OK && t == 250);
}

static void test_sensor_errors(void)
{
	struct ntc_config config = nominal;
	int16_t t = 777;

	CHECK(ntc_convert(0, &config, &t) == NTC_SHORT && t == 777);
	CHECK(ntc_convert(4095, &config, &t) == NTC_SATURATED && t == 777);
	CHECK(ntc_convert(1, &config, &t) == NTC_OUT_OF_RANGE && t == 777);
	CHECK(ntc_convert(4094, &config, &t) == NTC_OUT_OF_RANGE && t == 777);
	config.pullup_mv = 1000;
	config.reference_mv = 2000;
	CHECK(ntc_convert(2048, &config, &t) == NTC_SATURATED && t == 777);
	/* 分母恰好为0也必须拒绝。 */
	config.pullup_mv = 1000;
	config.reference_mv = 3000;
	CHECK(ntc_convert(1365, &config, &t) == NTC_SATURATED && t == 777);
}

static void test_adc_sweep(void)
{
	int16_t previous = 1201;
	unsigned int valid = 0;
	int left_valid_range = 0;

	for (uint16_t raw = 1; raw < NTC_ADC_MAX; ++raw) {
		int16_t t = 9999;
		enum ntc_status status = ntc_convert(raw, &nominal, &t);

		if (status == NTC_OK) {
			CHECK(!left_valid_range);
			CHECK(t >= -200 && t <= 1200 && t <= previous);
			previous = t;
			++valid;
		} else {
			CHECK(status == NTC_OUT_OF_RANGE && t == 9999);
			if (valid) {
				left_valid_range = 1;
			}
		}
	}
	CHECK(valid > 3500 && left_valid_range);
}

static void test_invalid_args(void)
{
	struct ntc_config config = nominal;
	int16_t t = -999;

	CHECK(ntc_convert(1456, NULL, &t) == NTC_INVALID_ARGUMENT && t == -999);
	CHECK(ntc_convert(1456, &config, NULL) == NTC_INVALID_ARGUMENT);
	CHECK(ntc_convert(UINT16_MAX, &config, &t) == NTC_INVALID_ARGUMENT && t == -999);
	CHECK(ntc_from_resistance(100000, NULL) == NTC_INVALID_ARGUMENT);
	config.pullup_ohm = 0;
	CHECK(ntc_convert(1456, &config, &t) == NTC_INVALID_ARGUMENT && t == -999);
	config.pullup_ohm = UINT32_MAX;
	CHECK(ntc_convert(1456, &config, &t) == NTC_INVALID_ARGUMENT && t == -999);
	config = nominal;
	config.pullup_mv = 0;
	CHECK(ntc_convert(1456, &config, &t) == NTC_INVALID_ARGUMENT && t == -999);
	config = nominal;
	config.reference_mv = 0;
	CHECK(ntc_convert(1456, &config, &t) == NTC_INVALID_ARGUMENT && t == -999);
}

static void test_wide_arithmetic(void)
{
	struct ntc_config config = {1000000, UINT16_MAX, UINT16_MAX};
	int16_t t = -999;

	CHECK(ntc_convert(4094, &config, &t) == NTC_OUT_OF_RANGE && t == -999);
	CHECK(ntc_convert(4, &config, &t) == NTC_OK && t > 870 && t < 890);
	config.pullup_ohm = 22000;
	CHECK(ntc_convert(1280, &config, &t) == NTC_OK && t == 250);
}

int main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } cases[] = {
		{"anchors", test_anchors}, {"interpolation", test_interpolation},
		{"limits", test_limits}, {"nominal_adc", test_nominal_adc},
		{"reference_config", test_reference_config}, {"sensor_errors", test_sensor_errors},
		{"adc_sweep", test_adc_sweep}, {"invalid_args", test_invalid_args},
		{"wide_arithmetic", test_wide_arithmetic}
	};
	size_t ran = 0;

	CHECK(argc <= 2);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		if (argc == 1 || strcmp(argv[1], cases[i].name) == 0) {
			cases[i].run();
			printf("PASS %s\n", cases[i].name);
			++ran;
		}
	}
	CHECK(ran != 0);
	return EXIT_SUCCESS;
}
