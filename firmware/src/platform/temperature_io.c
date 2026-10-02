/* @brief ADC1单端软件触发读三路NTC，所有硬件等待有截止时间。 */
#include "platform/temperature_io.h"
#include "platform/board_io.h"
#include "config/analog.h"

#include <errno.h>
#include <soc.h>
#include <stm32_ll_adc.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_gpio.h>
#include <stm32_ll_system.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define ADC_WAIT_MS 5
#define ADC_PAIR_MAX_US 1000u
#define ADC_ACTIVITY (ADC_CR_ADEN | ADC_CR_ADSTART | ADC_CR_JADSTART | ADC_CR_ADCAL)

BUILD_ASSERT(IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER), "ADC pair timing requires 64-bit cycles");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(adc1), okay), "ADC1 must remain LL-owned");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(adc2), okay), "ADC12 is reserved for slow sampling");
BUILD_ASSERT(HT_VREF_MV == 2900u, "Board VREFBUF supplies 2.9 V");
BUILD_ASSERT(HT_NTC_PULLUP_OHM > 0 && HT_NTC_PULLUP_OHM <= 1000000u, "Invalid NTC pullup");
BUILD_ASSERT(HT_NTC_PULLUP_MV >= 3135u && HT_NTC_PULLUP_MV <= 3600u,
	     "AVDD configuration must support the 2.9 V buffer");

static const uint32_t channels[TEMPERATURE_CHANNEL_COUNT] = {
	LL_ADC_CHANNEL_9, LL_ADC_CHANNEL_2, LL_ADC_CHANNEL_1
};
static const struct ntc_config config = {
	.pullup_ohm = HT_NTC_PULLUP_OHM,
	.pullup_mv = HT_NTC_PULLUP_MV,
	.reference_mv = HT_VREF_MV
};
static bool owned;
static bool ready;
static uint32_t expected_cfgr;

static int wait_bits(volatile const uint32_t *reg, uint32_t mask, uint32_t expected)
{
	int64_t deadline = k_uptime_get() + ADC_WAIT_MS;

	while ((*reg & mask) != expected) {
		if (k_uptime_get() >= deadline) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}
	return 0;
}

static bool pins_available(void)
{
	return LL_GPIO_GetPinMode(GPIOC, LL_GPIO_PIN_3) == LL_GPIO_MODE_ANALOG &&
	       LL_GPIO_GetPinPull(GPIOC, LL_GPIO_PIN_3) == LL_GPIO_PULL_NO &&
	       LL_GPIO_GetPinMode(GPIOA, LL_GPIO_PIN_1) == LL_GPIO_MODE_ANALOG &&
	       LL_GPIO_GetPinPull(GPIOA, LL_GPIO_PIN_1) == LL_GPIO_PULL_NO &&
	       LL_GPIO_GetPinMode(GPIOA, LL_GPIO_PIN_0) == LL_GPIO_MODE_ANALOG &&
	       LL_GPIO_GetPinPull(GPIOA, LL_GPIO_PIN_0) == LL_GPIO_PULL_NO;
}

static void invalidate(struct temperature_snapshot *snapshot, enum ntc_status status)
{
	*snapshot = (struct temperature_snapshot){0};
	for (size_t i = 0; i < TEMPERATURE_CHANNEL_COUNT; ++i) {
		snapshot->samples[i].status = status;
	}
}

void temperature_io_stop(void)
{
	ready = false;
	if (!owned) {
		return;
	}
	if (LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_ADC12)) {
		if (LL_ADC_REG_IsConversionOngoing(ADC1)) {
			LL_ADC_REG_StopConversion(ADC1);
			(void)wait_bits(&ADC1->CR, ADC_CR_ADSTART | ADC_CR_ADSTP, 0);
		}
		if (LL_ADC_IsEnabled(ADC1) && !(ADC1->CR & (ADC_CR_ADSTART | ADC_CR_JADSTART))) {
			LL_ADC_Disable(ADC1);
			(void)wait_bits(&ADC1->CR, ADC_CR_ADEN | ADC_CR_ADDIS, 0);
		}
		/* 仅在ADC2仍未被外部启用时复位整个共有块，含超时清理。 */
		if (!(ADC2->CR & ADC_ACTIVITY)) {
			LL_AHB2_GRP1_ForceReset(LL_AHB2_GRP1_PERIPH_ADC12);
			LL_AHB2_GRP1_ReleaseReset(LL_AHB2_GRP1_PERIPH_ADC12);
			LL_AHB2_GRP1_DisableClock(LL_AHB2_GRP1_PERIPH_ADC12);
		}
	}
	owned = false;
}

int temperature_io_init(void)
{
	int rc;
	uint32_t revision = LL_DBGMCU_GetRevisionID();

	if (owned || ready || LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_ADC12)) {
		return -EBUSY;
	}
	/* ES0430 2.7.8还影响Rev Z；当前仅验证不受此项影响的Y/X。 */
	if (revision != 0x2002u && revision != 0x2003u) {
		return -ENOTSUP;
	}
	if (SystemCoreClock != 170000000u) {
		return -EINVAL;
	}
	LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA | LL_AHB2_GRP1_PERIPH_GPIOC);
	if (!pins_available()) {
		return -EBUSY;
	}
	rc = board_io_start_reference();
	if (rc != 0) {
		return rc;
	}
	LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_ADC12);
	/* 时钟原先关闭也可能残留外部配置，必须先检查，不能先复位。 */
	if ((ADC1->CR | ADC2->CR) & ADC_ACTIVITY) {
		LL_AHB2_GRP1_DisableClock(LL_AHB2_GRP1_PERIPH_ADC12);
		return -EBUSY;
	}
	owned = true;
	LL_AHB2_GRP1_ForceReset(LL_AHB2_GRP1_PERIPH_ADC12);
	LL_AHB2_GRP1_ReleaseReset(LL_AHB2_GRP1_PERIPH_ADC12);
	LL_ADC_SetCommonClock(ADC12_COMMON, LL_ADC_CLOCK_SYNC_PCLK_DIV4);
	LL_ADC_DisableDeepPowerDown(ADC1);
	LL_ADC_EnableInternalRegulator(ADC1);
	k_busy_wait(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US);
	LL_ADC_SetResolution(ADC1, LL_ADC_RESOLUTION_12B);
	LL_ADC_SetDataAlignment(ADC1, LL_ADC_DATA_ALIGN_RIGHT);
	LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_SOFTWARE);
	LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_DISABLE);
	LL_ADC_REG_SetContinuousMode(ADC1, LL_ADC_REG_CONV_SINGLE);
	LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_NONE);
	for (size_t i = 0; i < TEMPERATURE_CHANNEL_COUNT; ++i) {
		LL_ADC_SetChannelSingleDiff(ADC1, channels[i], LL_ADC_SINGLE_ENDED);
		LL_ADC_SetChannelSamplingTime(ADC1, channels[i], LL_ADC_SAMPLINGTIME_640CYCLES_5);
	}
	expected_cfgr = ADC1->CFGR;
	LL_ADC_StartCalibration(ADC1, LL_ADC_SINGLE_ENDED);
	rc = wait_bits(&ADC1->CR, ADC_CR_ADCAL, 0);
	if (rc == 0) {
		/* 1us超过42.5MHz下要求的4个ADC时钟周期。 */
		k_busy_wait(1);
		LL_ADC_ClearFlag_ADRDY(ADC1);
		LL_ADC_Enable(ADC1);
		rc = wait_bits(&ADC1->ISR, ADC_ISR_ADRDY, ADC_ISR_ADRDY);
	}
	if (rc != 0) {
		temperature_io_stop();
		return rc;
	}
	ready = true;
	return 0;
}

static int check_adc(void)
{
	if (SystemCoreClock != 170000000u || !LL_VREFBUF_IsVREFReady() ||
	    !LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_ADC12)) {
		return -EIO;
	}
	/* 检出常见外部接管：不允许触发、DMA、双ADC或注入转换并用。 */
	if ((ADC2->CR & ADC_ACTIVITY) || !LL_ADC_IsEnabled(ADC1) ||
	    (ADC1->CR & (ADC_CR_ADSTART | ADC_CR_JADSTART | ADC_CR_ADCAL)) ||
	    ADC1->CFGR != expected_cfgr || ADC1->CFGR2 != 0 || ADC1->JSQR != 0 || ADC1->IER != 0 ||
	    ADC1->DIFSEL != 0 || ADC12_COMMON->CCR != LL_ADC_CLOCK_SYNC_PCLK_DIV4 ||
	    LL_ADC_REG_GetSequencerLength(ADC1) != LL_ADC_REG_SEQ_SCAN_DISABLE ||
	    !pins_available()) {
		return -EBUSY;
	}
	for (size_t i = 0; i < TEMPERATURE_CHANNEL_COUNT; ++i) {
		if (LL_ADC_GetChannelSamplingTime(ADC1, channels[i]) != LL_ADC_SAMPLINGTIME_640CYCLES_5) {
			return -EBUSY;
		}
	}
	return 0;
}

static int read_once(uint16_t *raw)
{
	LL_ADC_ClearFlag_EOC(ADC1);
	LL_ADC_ClearFlag_EOS(ADC1);
	LL_ADC_ClearFlag_OVR(ADC1);
	LL_ADC_REG_StartConversion(ADC1);
	int rc = wait_bits(&ADC1->ISR, ADC_ISR_EOC, ADC_ISR_EOC);

	if (rc == 0) {
		rc = wait_bits(&ADC1->CR, ADC_CR_ADSTART, 0);
	}
	if (rc == 0 && LL_ADC_IsActiveFlag_OVR(ADC1)) {
		rc = -EIO;
	}
	if (rc == 0) {
		*raw = LL_ADC_REG_ReadConversionData12(ADC1);
	}
	return rc;
}

int temperature_io_read(struct temperature_snapshot *snapshot)
{
	int rc;

	if (snapshot == NULL) {
		return -EINVAL;
	}
	invalidate(snapshot, NTC_NOT_READY);
	if (!ready) {
		return -EACCES;
	}
	rc = check_adc();
	for (size_t i = 0; rc == 0 && i < TEMPERATURE_CHANNEL_COUNT; ++i) {
		uint16_t raw = 0;

		LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, channels[i]);
		/* ES0430 2.7.9：空闲后的首笔丢弃；整个采样对须小于1ms。
		 * 中断始终开放，调度/ISR延迟过长只会使结果失效，不接受旧首笔。
		 */
		uint64_t start = k_cycle_get_64();

		rc = read_once(&raw);
		if (rc == 0) {
			rc = read_once(&raw);
		}
		if (rc == 0 && k_cycle_get_64() - start >= k_us_to_cyc_ceil64(ADC_PAIR_MAX_US)) {
			rc = -ETIMEDOUT;
		}
		if (rc == 0) {
			struct temperature_sample *sample = &snapshot->samples[i];

			sample->raw = raw;
			sample->status = ntc_convert(sample->raw, &config, &sample->decicelsius);
		}
	}
	if (rc != 0) {
		invalidate(snapshot, NTC_IO_ERROR);
		temperature_io_stop();
	}
	return rc;
}
