/* @brief 双ADC持续采集，待机仅开采样时钟；NTC维护后丢弃首半区。 */
#include "platform/signal_io.h"
#include "platform/dds_io.h"
#include "platform/board_io.h"
#include "config/analog.h"
#include "core/rms.h"
#include <errno.h>
#include <string.h>
#include <soc.h>
#include <stm32_ll_adc.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_dma.h>
#include <stm32_ll_gpio.h>
#include <stm32_ll_system.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#define HALF 850u
#define IDLE_FREQUENCY 2000u
#define ADC_DMA (DMA_CCR_CIRC | DMA_CCR_MINC | DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1 | DMA_CCR_PL_1)
struct block { uint32_t number; uint32_t samples[HALF]; };
K_MSGQ_DEFINE(blocks, sizeof(uint8_t), 3, 4);
static uint32_t buffer[HALF * 2] __aligned(4);
/* 三块排队，一块供线程处理；队列临界区只复制槽号。 */
static struct block pool[4];
static atomic_t busy;
static struct rms accumulator;
static struct signal_snapshot value;
static bool ready, discard;
static volatile bool active, output;
static volatile bool failed;
static uint32_t number, expected, windows;
static int64_t next_temperature;
static uint32_t adc_config[2], common_config;

BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(adc1), okay), "ADC12 LL owner");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(adc2), okay), "ADC12 LL owner");

static int wait_bits(volatile const uint32_t *reg, uint32_t bits, uint32_t want)
{
    int64_t end = k_uptime_get() + 5;
    while ((*reg & bits) != want) {
        if (k_uptime_get() >= end) return -ETIMEDOUT;
        k_busy_wait(1);
    }
    return 0;
}

void signal_io_fault(void)
{
    failed = true;
    active = output = false;
    dds_io_fault_stop();
    if (ready) {
        ADC1->IER = 0;
        ADC2->IER = 0;
        DMA1_Channel2->CCR &= ~DMA_CCR_EN;
    }
}

bool signal_io_failed(void) { return failed; }

static bool resources_ready(void)
{
    if (!LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_ADC12)) return false;
    if (ADC1->CFGR != adc_config[0] || ADC2->CFGR != adc_config[1] ||
        ADC12_COMMON->CCR != common_config || ADC1->DIFSEL != BIT(6) || ADC2->DIFSEL != BIT(3) ||
        !(ADC1->CR & ADC_CR_ADEN) || !(ADC2->CR & ADC_CR_ADEN) ||
        __LL_ADC_CHANNEL_TO_DECIMAL_NB(LL_ADC_REG_GetSequencerRanks(ADC1, LL_ADC_REG_RANK_1)) != 6 ||
        __LL_ADC_CHANNEL_TO_DECIMAL_NB(LL_ADC_REG_GetSequencerRanks(ADC2, LL_ADC_REG_RANK_1)) != 3 ||
        DMA1_Channel2->CPAR != (uint32_t)(uintptr_t)&ADC12_COMMON->CDR ||
        DMA1_Channel2->CMAR != (uint32_t)(uintptr_t)buffer ||
        DMAMUX1_Channel1->CCR != LL_DMAMUX_REQ_ADC1) return false;
    const uint32_t pins[] = {LL_GPIO_PIN_0, LL_GPIO_PIN_1, LL_GPIO_PIN_6, LL_GPIO_PIN_7,
                            LL_GPIO_PIN_0, LL_GPIO_PIN_1, LL_GPIO_PIN_3};
    for (size_t i = 0; i < ARRAY_SIZE(pins); ++i) {
        GPIO_TypeDef *port = i < 4 ? GPIOA : GPIOC;
        if (LL_GPIO_GetPinMode(port, pins[i]) != LL_GPIO_MODE_ANALOG ||
            LL_GPIO_GetPinPull(port, pins[i]) != LL_GPIO_PULL_NO) return false;
    }
    uint32_t dma = ADC_DMA;
    if (active) dma |= DMA_CCR_EN | DMA_CCR_TEIE | DMA_CCR_HTIE | DMA_CCR_TCIE;
    return DMA1_Channel2->CCR == dma && DMA1_Channel2->CNDTR <= HALF * 2;
}

static void dma_interrupt(const void *ctx)
{
    ARG_UNUSED(ctx);
    bool half = LL_DMA_IsActiveFlag_HT2(DMA1);
    bool full = LL_DMA_IsActiveFlag_TC2(DMA1);
    if (failed || !active) { DMA1->IFCR = DMA_IFCR_CGIF2; return; }
    if (LL_DMA_IsActiveFlag_TE2(DMA1) || (half && full)) { signal_io_fault(); return; }
    if (!half && !full) return;
    if (half) LL_DMA_ClearFlag_HT2(DMA1); else LL_DMA_ClearFlag_TC2(DMA1);
    uint32_t remaining = DMA1_Channel2->CNDTR;
    if (remaining == 0 || (half ? remaining > HALF : remaining <= HALF)) { signal_io_fault(); return; }
    uint8_t slot = number % ARRAY_SIZE(pool);
    if (atomic_test_and_set_bit(&busy, slot)) { signal_io_fault(); return; }
    struct block *item = &pool[slot];
    item->number = ++number;
    memcpy(item->samples, buffer + (half ? 0 : HALF), sizeof(item->samples));
    __DMB();
    remaining = DMA1_Channel2->CNDTR;
    if (failed || remaining == 0 || (half ? remaining > HALF : remaining <= HALF) ||
        LL_DMA_IsActiveFlag_HT2(DMA1) || LL_DMA_IsActiveFlag_TC2(DMA1) ||
        k_msgq_put(&blocks, &slot, K_NO_WAIT) != 0) {
        atomic_clear_bit(&busy, slot);
        signal_io_fault();
    }
}

static void adc_interrupt(const void *ctx)
{
    ARG_UNUSED(ctx);
    if ((ADC1->ISR | ADC2->ISR) & ADC_ISR_OVR) signal_io_fault();
}

static int pause_adc(void)
{
    irq_disable(DMA1_Channel2_IRQn);
    ADC1->IER = 0;
    ADC2->IER = 0;
    if (LL_ADC_REG_IsConversionOngoing(ADC1)) LL_ADC_REG_StopConversion(ADC1);
    int rc = wait_bits(&ADC1->CR, ADC_CR_ADSTART | ADC_CR_ADSTP, 0);
    if (rc == 0) rc = wait_bits(&ADC2->CR, ADC_CR_ADSTART | ADC_CR_ADSTP, 0);
    DMA1_Channel2->CCR = ADC_DMA;
    if (rc == 0) rc = wait_bits(&DMA1_Channel2->CCR, DMA_CCR_EN, 0);
    DMA1->IFCR = DMA_IFCR_CGIF2;
    k_msgq_purge(&blocks);
    atomic_clear(&busy);
    rms_reset(&accumulator);
    expected = number;
    discard = true;
    return rc;
}

static int arm_adc(void)
{
    uint32_t key = irq_lock();
    if (failed) { irq_unlock(key); return -EIO; }
    DMA1->IFCR = DMA_IFCR_CGIF2;
    DMA1_Channel2->CNDTR = HALF * 2;
    LL_ADC_ClearFlag_OVR(ADC1);
    LL_ADC_ClearFlag_OVR(ADC2);
    LL_ADC_ClearFlag_EOC(ADC1);
    LL_ADC_ClearFlag_EOS(ADC1);
    ADC1->IER = ADC_IER_OVRIE;
    ADC2->IER = ADC_IER_OVRIE;
    DMA1_Channel2->CCR = ADC_DMA | DMA_CCR_EN | DMA_CCR_TEIE | DMA_CCR_HTIE | DMA_CCR_TCIE;
    NVIC_ClearPendingIRQ(DMA1_Channel2_IRQn);
    irq_enable(DMA1_Channel2_IRQn);
    LL_ADC_REG_StartConversion(ADC1);
    irq_unlock(key);
    return failed ? -EIO : 0;
}

static int stop(void)
{
    int rc = dds_io_stop();
    if (ready) {
        int next = pause_adc();
        if (rc == 0) rc = next;
    }
    active = output = false;
    value.reading.valid = false;
    return rc;
}

static int sample(void)
{
    active = true;
    int rc = arm_adc();
    if (rc == 0) rc = dds_io_sample_start(IDLE_FREQUENCY);
    if (rc != 0) signal_io_fault();
    return rc;
}

static int configure_adc(ADC_TypeDef *adc, uint32_t channel)
{
    LL_ADC_DisableDeepPowerDown(adc);
    LL_ADC_EnableInternalRegulator(adc);
    k_busy_wait(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US);
    LL_ADC_SetResolution(adc, LL_ADC_RESOLUTION_12B);
    LL_ADC_SetDataAlignment(adc, LL_ADC_DATA_ALIGN_RIGHT);
    LL_ADC_SetChannelSingleDiff(adc, channel, LL_ADC_DIFFERENTIAL_ENDED);
    LL_ADC_SetChannelSamplingTime(adc, channel, LL_ADC_SAMPLINGTIME_24CYCLES_5);
    LL_ADC_REG_SetSequencerLength(adc, LL_ADC_REG_SEQ_SCAN_DISABLE);
    LL_ADC_REG_SetSequencerRanks(adc, LL_ADC_REG_RANK_1, channel);
    LL_ADC_REG_SetContinuousMode(adc, LL_ADC_REG_CONV_SINGLE);
    /* ES0430 2.7.6：双ADC共用DMA时关闭DR FIFO，保留OVR中断报错。 */
    LL_ADC_REG_SetOverrun(adc, LL_ADC_REG_OVR_DATA_OVERWRITTEN);
    LL_ADC_StartCalibration(adc, LL_ADC_SINGLE_ENDED);
    int rc = wait_bits(&adc->CR, ADC_CR_ADCAL, 0);
    if (rc == 0) {
        k_busy_wait(1);
        LL_ADC_StartCalibration(adc, LL_ADC_DIFFERENTIAL_ENDED);
        rc = wait_bits(&adc->CR, ADC_CR_ADCAL, 0);
    }
    if (rc == 0) {
        k_busy_wait(1);
        LL_ADC_ClearFlag_ADRDY(adc);
        LL_ADC_Enable(adc);
        rc = wait_bits(&adc->ISR, ADC_ISR_ADRDY, ADC_ISR_ADRDY);
    }
    return rc;
}

int signal_io_init(void)
{
    if (ready || failed || LL_AHB2_GRP1_IsEnabledClock(LL_AHB2_GRP1_PERIPH_ADC12)) return -EBUSY;
    uintptr_t begin = (uintptr_t)buffer;
    if (begin < SRAM1_BASE || begin > SRAM2_BASE + SRAM2_SIZE - sizeof(buffer)) return -EFAULT;
    uint32_t revision = LL_DBGMCU_GetRevisionID();
    if (revision != 0x2002 && revision != 0x2003) return -ENOTSUP;
    int rc = board_io_start_reference();
    if (rc == 0) rc = dds_io_init();
    if (rc != 0) return rc;
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_ADC12);
    if ((ADC1->CR | ADC2->CR) & (ADC_CR_ADEN | ADC_CR_ADSTART | ADC_CR_JADSTART | ADC_CR_ADCAL)) {
        signal_io_fault(); return -EBUSY;
    }
    if (DMA1_Channel2->CCR & DMA_CCR_EN) { signal_io_fault(); return -EBUSY; }
    LL_AHB2_GRP1_ForceReset(LL_AHB2_GRP1_PERIPH_ADC12);
    LL_AHB2_GRP1_ReleaseReset(LL_AHB2_GRP1_PERIPH_ADC12);
    LL_ADC_SetCommonClock(ADC12_COMMON, LL_ADC_CLOCK_SYNC_PCLK_DIV4);
    LL_ADC_SetMultimode(ADC12_COMMON, LL_ADC_MULTI_DUAL_REG_SIMULT);
    LL_ADC_SetMultiDMATransfer(ADC12_COMMON, LL_ADC_MULTI_REG_DMA_UNLMT_RES12_10B);
    LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_EXT_TIM6_TRGO);
    LL_ADC_REG_SetTriggerEdge(ADC1, LL_ADC_REG_TRIG_EXT_RISING);
    LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);
    const uint32_t channels[] = {LL_ADC_CHANNEL_9, LL_ADC_CHANNEL_2, LL_ADC_CHANNEL_1};
    for (size_t i = 0; i < 3; ++i)
        LL_ADC_SetChannelSamplingTime(ADC1, channels[i], LL_ADC_SAMPLINGTIME_640CYCLES_5);
    ADC1->CFGR |= ADC_CFGR_JQDIS;
    rc = configure_adc(ADC1, LL_ADC_CHANNEL_6);
    if (rc == 0) rc = configure_adc(ADC2, LL_ADC_CHANNEL_3);
    if (rc != 0) { signal_io_fault(); return rc; }
    DMA1_Channel2->CCR = ADC_DMA;
    DMA1_Channel2->CPAR = (uint32_t)(uintptr_t)&ADC12_COMMON->CDR;
    DMA1_Channel2->CMAR = (uint32_t)(uintptr_t)buffer;
    DMAMUX1_Channel1->CCR = LL_DMAMUX_REQ_ADC1;
    adc_config[0] = ADC1->CFGR;
    adc_config[1] = ADC2->CFGR;
    common_config = ADC12_COMMON->CCR;
    IRQ_CONNECT(DMA1_Channel2_IRQn, 1, dma_interrupt, NULL, 0);
    IRQ_CONNECT(ADC1_2_IRQn, 0, adc_interrupt, NULL, 0);
    irq_enable(ADC1_2_IRQn);
    ready = true;
    if (!resources_ready()) { signal_io_fault(); return -EIO; }
    rc = pause_adc();
    if (rc == 0) rc = sample();
    if (rc != 0) signal_io_fault();
    return rc;
}

int signal_io_start(uint32_t frequency)
{
    if (!ready || failed || output) return -EACCES;
    int rc = stop();
    if (rc == 0) rc = dds_io_configure(frequency, 0);
    if (rc != 0) { signal_io_fault(); return rc; }
    active = true;
    rc = arm_adc();
    if (rc == 0) rc = dds_io_start();
    if (rc != 0) { signal_io_fault(); return rc; }
    output = true;
    if (failed) { output = false; return -EIO; }
    return 0;
}

int signal_io_stop(void)
{
    int rc = stop();
    if (rc != 0) signal_io_fault();
    if (rc == 0 && ready && !failed) rc = sample();
    return rc;
}

static int temperature(void)
{
    int rc = pause_adc();
    const uint32_t channels[] = {LL_ADC_CHANNEL_9, LL_ADC_CHANNEL_2, LL_ADC_CHANNEL_1};
    const struct ntc_config config = {HT_NTC_PULLUP_OHM, HT_NTC_PULLUP_MV, HT_VREF_MV};
    for (size_t i = 0; rc == 0 && i < 3; ++i) {
        uint16_t raw = 0;
        uint64_t began = k_cycle_get_64();
        ADC1->JSQR = 0;
        LL_ADC_INJ_SetSequencerRanks(ADC1, LL_ADC_INJ_RANK_1, channels[i]);
        for (size_t pass = 0; rc == 0 && pass < 2; ++pass) {
            LL_ADC_ClearFlag_JEOS(ADC1);
            LL_ADC_INJ_StartConversion(ADC1);
            rc = wait_bits(&ADC1->ISR, ADC_ISR_JEOS, ADC_ISR_JEOS);
            if (rc == 0) rc = wait_bits(&ADC1->CR, ADC_CR_JADSTART, 0);
            if (rc == 0) raw = LL_ADC_INJ_ReadConversionData12(ADC1, LL_ADC_INJ_RANK_1);
        }
        if (rc == 0 && k_cycle_get_64() - began >= k_us_to_cyc_ceil64(1000)) rc = -ETIMEDOUT;
        value.temperature.samples[i].raw = raw;
        value.temperature.samples[i].status = rc == 0 ?
            ntc_convert(raw, &config, &value.temperature.samples[i].decicelsius) : NTC_IO_ERROR;
    }
    if (rc == 0) {
        value.temperature_ms = k_uptime_get();
        if (active) rc = arm_adc();
    }
    return rc;
}

int signal_io_poll(struct signal_snapshot *out)
{
    if (!out || !ready || failed) return -EIO;
    if (dds_io_check() != 0 || !resources_ready()) { signal_io_fault(); return -EIO; }
    uint8_t slot;
    for (unsigned int n = 0; n < 3 && k_msgq_get(&blocks, &slot, K_NO_WAIT) == 0; ++n) {
        if (slot >= ARRAY_SIZE(pool) || !atomic_test_bit(&busy, slot) ||
            pool[slot].number != ++expected) { signal_io_fault(); return -EIO; }
        if (discard) { discard = false; atomic_clear_bit(&busy, slot); continue; }
        int rc = rms_add(&accumulator, pool[slot].samples, HALF);
        atomic_clear_bit(&busy, slot);
        if (rc == 0 && accumulator.count == 8500) {
            rc = rms_result(&accumulator, &value.reading);
            if (rc == 0) {
                value.reading.sequence = ++windows;
                value.reading.time_ms = k_uptime_get();
            }
            rms_reset(&accumulator);
        }
        if (rc != 0) {
            value.reading.valid = false;
            rms_reset(&accumulator);
            if (output) { signal_io_fault(); return -ERANGE; }
        }
    }
    if (k_uptime_get() >= next_temperature) {
        next_temperature = k_uptime_get() + 200;
        int rc = temperature();
        if (rc != 0) { signal_io_fault(); return rc; }
    }
    if (failed) return -EIO;
    *out = value;
    return 0;
}
