/* @brief 独占DAC1/TIM6/DMA1通道1输出不可变波表，错误锁存并直接静音。 */
#include "platform/dds_io.h"
#include "core/dds.h"

#include <errno.h>
#include <soc.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_dac.h>
#include <stm32_ll_dma.h>
#include <stm32_ll_gpio.h>
#include <stm32_ll_rcc.h>
#include <stm32_ll_system.h>
#include <stm32_ll_tim.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define DMA_SETTINGS (DMA_CCR_DIR | DMA_CCR_CIRC | DMA_CCR_MINC | \
		      DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_PL_1)
#define DAC_TRIGGER LL_DAC_TRIG_EXT_TIM6_TRGO
#define DMA_CLOCKS (LL_AHB1_GRP1_PERIPH_DMA1 | LL_AHB1_GRP1_PERIPH_DMAMUX1)
#define STOP_POLLS 128u

BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(dac1), okay), "DAC1 is LL-owned");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(dac3), okay), "TIM6_DAC IRQ is reserved");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(timers6), okay), "TIM6 is LL-owned");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(dma1), okay), "DMA1 is LL-owned");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(dmamux1), okay), "DMAMUX is LL-owned");
BUILD_ASSERT(DDS_BUFFER_SAMPLES <= UINT16_MAX, "DMA counter is 16-bit");

static uint16_t samples[DDS_BUFFER_SAMPLES] __aligned(4);
static K_MUTEX_DEFINE(lock);
static bool owned;
static bool configured;
static volatile bool running;
static volatile enum dds_fault fault;
static uint32_t frequency;
static uint16_t amplitude;
static uint16_t timer_arr;

static bool clocks_ready(void)
{
	return LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_DAC1) &&
	       LL_APB1_GRP1_IsEnabledClock(LL_APB1_GRP1_PERIPH_TIM6) &&
	       LL_AHB1_GRP1_IsEnabledClock(DMA_CLOCKS);
}

static bool reference_ready(void)
{
	return SystemCoreClock == 170000000u &&
	       LL_RCC_GetAHBPrescaler() == LL_RCC_SYSCLK_DIV_1 &&
	       LL_RCC_GetAPB1Prescaler() == LL_RCC_APB1_DIV_1 &&
	       LL_VREFBUF_IsVREFReady() &&
	       LL_VREFBUF_GetVoltageScaling() == LL_VREFBUF_VOLTAGE_SCALE2 &&
	       (VREFBUF->CSR & (VREFBUF_CSR_ENVR | VREFBUF_CSR_HIZ)) == VREFBUF_CSR_ENVR &&
	       LL_GPIO_GetPinMode(GPIOA, LL_GPIO_PIN_4) == LL_GPIO_MODE_ANALOG &&
	       LL_GPIO_GetPinPull(GPIOA, LL_GPIO_PIN_4) == LL_GPIO_PULL_NO;
}

/* 调用者已屏蔽IRQ；只做有界寄存器操作，不等线程、不触碰ADC/VREF。 */
static bool quiet(void)
{
	running = false;
	if (!owned) {
		return true;
	}
	bool stopped = clocks_ready();
	bool dac_clock = LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_DAC1);
	bool dma_clock = LL_AHB1_GRP1_IsEnabledClock(LL_AHB1_GRP1_PERIPH_DMA1);

	/* 一个时钟被外部关闭也不妨碍停止其余仍能访问的输出资源。 */
	if (LL_APB1_GRP1_IsEnabledClock(LL_APB1_GRP1_PERIPH_TIM6)) {
		TIM6->CR1 = TIM_CR1_ARPE;
	}
	if (dac_clock) {
		DAC1->CR &= ~(DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1 | DAC_CR_TEN1);
	}
	if (dma_clock) {
		DMA1_Channel1->CCR &= ~(DMA_CCR_EN | DMA_CCR_TEIE);
		for (unsigned int i = 0; i < STOP_POLLS; ++i) {
			if (!(DMA1_Channel1->CCR & DMA_CCR_EN)) {
				break;
			}
		}
		stopped = stopped && !(DMA1_Channel1->CCR & DMA_CCR_EN);
		DMA1->IFCR = DMA_IFCR_CGIF1;
	}
	if (dac_clock) {
		DAC1->DHR12R1 = DDS_MIDPOINT;
		if (DAC1->CR & DAC_CR_EN1) {
			for (unsigned int i = 0; i < STOP_POLLS; ++i) {
				if (DAC1->DOR1 == DDS_MIDPOINT) {
					break;
				}
			}
			stopped = stopped && DAC1->DOR1 == DDS_MIDPOINT;
		}
		LL_DAC_ClearFlag_DMAUDR1(DAC1);
		if (!stopped) {
			/* 不能确认中点提交时关DAC，不把高阻误报成中点成功。 */
			LL_DAC_Disable(DAC1, LL_DAC_CHANNEL_1);
		}
	}
	return stopped;
}

static void fail(enum dds_fault reason)
{
	uint32_t key = __get_PRIMASK();

	__disable_irq();
	if (fault == DDS_FAULT_NONE) {
		fault = reason;
	}
	(void)quiet();
	__set_PRIMASK(key);
}

void dds_io_fault_stop(void)
{
	fail(DDS_FAULT_EXTERNAL);
}

static void dma_error(const void *context)
{
	(void)context;
	if (LL_DMA_IsActiveFlag_TE1(DMA1)) {
		fail(DDS_FAULT_DMA);
	}
}

static void dac_error(const void *context)
{
	(void)context;
	if (LL_DAC_IsActiveFlag_DMAUDR1(DAC1)) {
		fail(DDS_FAULT_DAC);
	}
}

static int check(void)
{
	if (!owned) {
		return -EACCES;
	}
	if (fault != DDS_FAULT_NONE) {
		return -EIO;
	}
	uint32_t dac = DAC_TRIGGER | (DAC1->CR & DAC_CR_EN1);
	uint32_t dma = DMA_SETTINGS;
	uint32_t timer = TIM_CR1_ARPE;

	if (running) {
		dac = DAC_TRIGGER | DAC_CR_EN1 | DAC_CR_TEN1 | DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1;
		dma |= DMA_CCR_EN | DMA_CCR_TEIE;
		timer |= TIM_CR1_CEN;
	}
	if (!clocks_ready() || !reference_ready() ||
	    LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_DAC3) ||
	    DAC1->CR != dac || DAC1->MCR != LL_DAC_HIGH_FREQ_MODE_ABOVE_160MHZ ||
	    TIM6->CR1 != timer || TIM6->CR2 != LL_TIM_TRGO_UPDATE || TIM6->DIER != 0 ||
	    TIM6->PSC != 0 || TIM6->ARR != timer_arr ||
	    DMA1_Channel1->CCR != dma || DMAMUX1_Channel0->CCR != LL_DMAMUX_REQ_DAC1_CH1 ||
	    DMA1_Channel1->CPAR != (uint32_t)(uintptr_t)&DAC1->DHR12R1 ||
	    DMA1_Channel1->CMAR != (uint32_t)(uintptr_t)samples ||
	    DMA1_Channel1->CNDTR > DDS_BUFFER_SAMPLES) {
		fail(DDS_FAULT_RESOURCE);
		return -EIO;
	}
	if (LL_DMA_IsActiveFlag_TE1(DMA1) || LL_DAC_IsActiveFlag_DMAUDR1(DAC1)) {
		fail(LL_DMA_IsActiveFlag_TE1(DMA1) ? DDS_FAULT_DMA : DDS_FAULT_DAC);
		return -EIO;
	}
	return fault == DDS_FAULT_NONE ? 0 : -EIO;
}

int dds_io_init(void)
{
	int rc = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (owned || fault != DDS_FAULT_NONE ||
	    LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_DAC1) ||
	    LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_DAC3) ||
	    LL_APB1_GRP1_IsEnabledClock(LL_APB1_GRP1_PERIPH_TIM6) ||
	    (RCC->AHB1ENR & DMA_CLOCKS)) {
		rc = -EBUSY;
		goto done;
	}
	if (!reference_ready()) {
		rc = -EACCES;
		goto done;
	}
	uintptr_t begin = (uintptr_t)samples;

	/* 限定在前96KiB SRAM1/2，不依赖CCM别名的DMA可达性。 */
	if (begin < SRAM1_BASE || begin > SRAM2_BASE + SRAM2_SIZE - sizeof(samples)) {
		rc = -EFAULT;
		goto done;
	}
	LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_DAC1);
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
	LL_AHB1_GRP1_EnableClock(DMA_CLOCKS);
	if (DAC1->CR != 0 || (TIM6->CR1 & TIM_CR1_CEN) ||
	    (DMA1_Channel1->CCR & DMA_CCR_EN)) {
		LL_AHB2_GRP1_DisableClock(LL_AHB2_GRP1_PERIPH_DAC1);
		LL_APB1_GRP1_DisableClock(LL_APB1_GRP1_PERIPH_TIM6);
		LL_AHB1_GRP1_DisableClock(DMA_CLOCKS);
		rc = -EBUSY;
		goto done;
	}
	owned = true;
	LL_AHB2_GRP1_ForceReset(LL_AHB2_GRP1_PERIPH_DAC1);
	LL_AHB2_GRP1_ReleaseReset(LL_AHB2_GRP1_PERIPH_DAC1);
	LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_TIM6);
	LL_APB1_GRP1_ReleaseReset(LL_APB1_GRP1_PERIPH_TIM6);
	DAC1->MCR = LL_DAC_HIGH_FREQ_MODE_ABOVE_160MHZ;
	DAC1->CR = DAC_TRIGGER;
	DAC1->DHR12R1 = DDS_MIDPOINT;
	(void)dds_timer(2000u, &timer_arr);
	TIM6->CR1 = TIM_CR1_ARPE;
	TIM6->CR2 = LL_TIM_TRGO_UPDATE;
	TIM6->PSC = 0;
	TIM6->ARR = timer_arr;
	TIM6->EGR = TIM_EGR_UG; /* DAC触发尚未使能；此UG不消耗波表。 */
	TIM6->SR = 0;
	DMA1_Channel1->CCR = DMA_SETTINGS;
	DMA1_Channel1->CPAR = (uint32_t)(uintptr_t)&DAC1->DHR12R1;
	DMA1_Channel1->CMAR = (uint32_t)(uintptr_t)samples;
	DMA1_Channel1->CNDTR = DDS_BUFFER_SAMPLES;
	DMAMUX1_Channel0->CCR = LL_DMAMUX_REQ_DAC1_CH1;
	DMA1->IFCR = DMA_IFCR_CGIF1;
	IRQ_CONNECT(DMA1_Channel1_IRQn, 1, dma_error, NULL, 0);
	IRQ_CONNECT(TIM6_DAC_IRQn, 1, dac_error, NULL, 0);
	NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
	NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
	irq_enable(DMA1_Channel1_IRQn);
	irq_enable(TIM6_DAC_IRQn);
done:
	k_mutex_unlock(&lock);
	return rc;
}

int dds_io_configure(uint32_t frequency_hz, uint16_t requested_amplitude)
{
	uint16_t arr;
	int rc;

	if (dds_timer(frequency_hz, &arr) != 0 || requested_amplitude > DDS_AMPLITUDE_MAX) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	rc = check();
	if (rc == 0 && running) {
		rc = -EBUSY;
	}
	if (rc == 0) {
		(void)dds_wave(samples, DDS_BUFFER_SAMPLES, requested_amplitude);
		frequency = frequency_hz;
		amplitude = requested_amplitude;
		timer_arr = arr;
		TIM6->ARR = arr;
		TIM6->CNT = 0;
		TIM6->EGR = TIM_EGR_UG;
		TIM6->SR = 0;
		configured = true;
		if (fault != DDS_FAULT_NONE) {
			rc = -EIO;
		}
	}
	k_mutex_unlock(&lock);
	return rc;
}

int dds_io_start(void)
{
	int rc;

	k_mutex_lock(&lock, K_FOREVER);
	rc = check();
	if (rc == 0 && (!configured || running)) {
		rc = running ? -EBUSY : -EACCES;
	}
	if (rc != 0) {
		goto done;
	}
	/* 起始DHR预载上一周期末点，第一触发输出该点并DMA装入sample[0]。
	 * 之后输出0..1699连续循环；不宣称与未来ADC已有相位配对。
	 */
	DAC1->DHR12R1 = DDS_MIDPOINT;
	LL_DAC_Enable(DAC1, LL_DAC_CHANNEL_1);
	k_busy_wait(LL_DAC_DELAY_STARTUP_VOLTAGE_SETTLING_US);
	if (!LL_DAC_IsReady(DAC1, LL_DAC_CHANNEL_1) || DAC1->DOR1 != DDS_MIDPOINT) {
		fail(DDS_FAULT_DAC);
		rc = -EIO;
		goto done;
	}
	DMA1_Channel1->CNDTR = DDS_BUFFER_SAMPLES;
	TIM6->CNT = 0;
	TIM6->SR = 0;
	DMA1->IFCR = DMA_IFCR_CGIF1;
	LL_DAC_ClearFlag_DMAUDR1(DAC1);
	uint32_t key = __get_PRIMASK();

	__disable_irq();
	if (fault == DDS_FAULT_NONE) {
		DAC1->CR |= DAC_CR_TEN1;
		DAC1->DHR12R1 = samples[DDS_BUFFER_SAMPLES - 1u];
		__DMB();
		DMA1_Channel1->CCR = DMA_SETTINGS | DMA_CCR_TEIE | DMA_CCR_EN;
		DAC1->CR |= DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1;
		running = true;
		TIM6->CR1 |= TIM_CR1_CEN;
	} else {
		rc = -EIO;
	}
	__set_PRIMASK(key);
	if (fault != DDS_FAULT_NONE) {
		rc = -EIO;
	}
done:
	k_mutex_unlock(&lock);
	return rc;
}

int dds_io_stop(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	uint32_t key = __get_PRIMASK();

	__disable_irq();
	bool stopped = quiet();

	if (!stopped && fault == DDS_FAULT_NONE) {
		fault = DDS_FAULT_STOP;
	}
	__set_PRIMASK(key);
	k_mutex_unlock(&lock);
	return stopped ? 0 : -EIO;
}

int dds_io_check(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	int rc = check();

	k_mutex_unlock(&lock);
	return rc;
}

int dds_io_snapshot(struct dds_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	uint32_t key = __get_PRIMASK();

	__disable_irq();
	*snapshot = (struct dds_snapshot){
		.ready = owned && fault == DDS_FAULT_NONE,
		.running = running,
		.frequency_hz = frequency,
		.amplitude = amplitude,
		.fault = fault
	};
	__set_PRIMASK(key);
	k_mutex_unlock(&lock);
	return 0;
}
