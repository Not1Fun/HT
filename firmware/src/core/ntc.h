/* @brief 用户阻温表的纯C换算，输出0.1摄氏度，不访问硬件。 */
#ifndef HT_NTC_H
#define HT_NTC_H

#include <stdint.h>

#define NTC_ADC_MAX 4095u

enum ntc_status {
	NTC_OK = 0,
	NTC_NOT_READY,
	NTC_INVALID_ARGUMENT,
	NTC_SHORT,
	NTC_SATURATED,
	NTC_OUT_OF_RANGE,
	NTC_IO_ERROR
};

struct ntc_config {
	uint32_t pullup_ohm; /* 1..1000000 ohm，实际装配值。 */
	uint16_t pullup_mv;
	uint16_t reference_mv; /* ADC VREF+；不等于NTC上拉电源。 */
};

/* 电阻单位0.1 ohm；表仅覆盖-20..120 C，不代表探头额定范围。
 * 两接口均只在NTC_OK时写输出；不外推，不把异常钳成有效温度。
 */
enum ntc_status ntc_from_resistance(uint32_t resistance_deciohm, int16_t *decicelsius);
/* 12位单端码：0报短路，4095报饱和；开路可能表现为饱和或超出表格。 */
enum ntc_status ntc_convert(uint16_t raw, const struct ntc_config *config,
			   int16_t *decicelsius);

#endif
