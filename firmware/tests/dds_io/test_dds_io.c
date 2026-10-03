/* @brief 包含生产驱动，只替换外设寄存器和RTOS边界。 */
#include "mock.h"
#include "../../src/platform/dds_io.c"
#include <stdio.h>

static void reset(void)
{
    memset(&hw_dac, 0, sizeof(hw_dac)); memset(&hw_timer, 0, sizeof(hw_timer));
    memset(&channel, 0, sizeof(channel)); memset(&hw_dma, 0, sizeof(hw_dma));
    memset(&rcc, 0, sizeof(rcc)); memset(&mux, 0, sizeof(mux));
    memset(handlers, 0, sizeof(handlers)); memset(irq_enabled, 0, sizeof(irq_enabled));
    owned = configured = running = sampling = false; fault = DDS_FAULT_NONE;
    software_triggers = 0;
    frequency = amplitude = timer_arr = 0; lock = 0; primask = 0;
    settle = dac_ready = vref_ready = true; dma_stuck = false; wait_hook = barrier_hook = NULL;
    SystemCoreClock = 170000000u; ahb_prescaler = apb_prescaler = pin_pull = 0;
    pin_mode = LL_GPIO_MODE_ANALOG; vref_scale = LL_VREFBUF_VOLTAGE_SCALE2;
    vref.CSR = VREFBUF_CSR_ENVR;
    sram_begin = (uintptr_t)samples; sram_end = sram_begin + sizeof(samples);
}

static struct dds_snapshot snapshot(void)
{
    struct dds_snapshot state;
    assert(dds_io_snapshot(&state) == 0);
    return state;
}

static void start(void)
{
    assert(dds_io_init() == 0);
    assert(dds_io_configure(2000, 70) == 0);
    assert(dds_io_start() == 0);
    assert(snapshot().running);
}

static void assert_quiet(enum dds_fault expected)
{
    struct dds_snapshot state = snapshot();
    assert(!state.running && state.fault == expected);
    assert(!(hw_timer.CR1 & TIM_CR1_CEN));
    assert(!(hw_dac.CR & (DAC_CR_TEN1 | DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1)));
    if (!dma_stuck) { assert(!(channel.CCR & DMA_CCR_EN)); }
    assert(!(channel.CCR & DMA_CCR_TEIE));
    assert(hw_dac.DHR12R1 == DDS_MIDPOINT);
    if (hw_dac.CR & DAC_CR_EN1) { assert(hw_dac.DOR1 == DDS_MIDPOINT); }
}

static void init_idle(void)
{
    assert(!snapshot().ready && !snapshot().running);
    assert(dds_io_init() == 0);
    assert(snapshot().ready && !snapshot().running);
    assert(dds_io_check() == 0);
    assert(!(hw_dac.CR & DAC_CR_EN1));
    assert_quiet(DDS_FAULT_NONE);
    assert(irq_enabled[0] && irq_enabled[1] && handlers[0] && handlers[1]);
    handlers[0](NULL); handlers[1](NULL);
    assert(snapshot().fault == DDS_FAULT_NONE);
    assert(dds_io_init() == -EBUSY);
}

static void input_guards(void)
{
    assert(dds_io_snapshot(NULL) == -EINVAL);
    assert(dds_io_configure(2000, 70) == -EACCES);
    assert(dds_io_start() == -EACCES);
    assert(dds_io_stop() == 0);
    assert(dds_io_init() == 0);
    assert(dds_io_start() == -EACCES);
    assert(dds_io_configure(2001, 70) == -EINVAL);
    assert(dds_io_configure(2000, 2048) == -EINVAL);
    assert(!configured && snapshot().frequency_hz == 0);
    assert(dds_io_configure(2000, 70) == 0 && dds_io_start() == 0);
    uint16_t before[DDS_BUFFER_SAMPLES]; memcpy(before, samples, sizeof(before));
    assert(dds_io_configure(10000, 90) == -EBUSY);
    assert(memcmp(before, samples, sizeof(before)) == 0);
    assert(snapshot().frequency_hz == 2000 && snapshot().amplitude == 70);
    assert(dds_io_start() == -EBUSY);
}

static void resource_conflicts(void)
{
    uint32_t *clock[] = {&rcc.AHB1ENR, &rcc.AHB1ENR, &rcc.AHB2ENR, &rcc.AHB2ENR, &rcc.APB1ENR};
    uint32_t mask[] = {LL_AHB1_GRP1_PERIPH_DMA1, LL_AHB1_GRP1_PERIPH_DMAMUX1,
        LL_AHB2_GRP1_PERIPH_DAC1, LL_AHB2_GRP1_PERIPH_DAC3, LL_APB1_GRP1_PERIPH_TIM6};
    for (size_t i = 0; i < 5; ++i) {
        reset(); *clock[i] = mask[i];
        assert(dds_io_init() == -EBUSY && !owned && *clock[i] == mask[i]);
    }
    reset(); hw_dac.CR = DAC_CR_EN1;
    assert(dds_io_init() == -EBUSY && hw_dac.CR == DAC_CR_EN1 && !owned);
    assert(rcc.AHB1ENR == 0 && rcc.AHB2ENR == 0 && rcc.APB1ENR == 0);
    reset(); hw_timer.CR1 = TIM_CR1_CEN;
    assert(dds_io_init() == -EBUSY && hw_timer.CR1 == TIM_CR1_CEN && !owned);
    reset(); channel.CCR = DMA_CCR_EN;
    assert(dds_io_init() == -EBUSY && channel.CCR == DMA_CCR_EN && !owned);
    reset(); sram_begin += 4;
    assert(dds_io_init() == -EFAULT && !owned);
    reset(); sram_end -= 4;
    assert(dds_io_init() == -EFAULT && !owned);
}

static void reference_guards(void)
{
    for (unsigned int i = 0; i < 8; ++i) {
        reset();
        switch (i) {
        case 0: SystemCoreClock = 160000000; break;
        case 1: ahb_prescaler = 1; break;
        case 2: apb_prescaler = 1; break;
        case 3: vref_ready = false; break;
        case 4: vref_scale = 1; break;
        case 5: vref.CSR |= VREFBUF_CSR_HIZ; break;
        case 6: pin_mode = 1; break;
        case 7: pin_pull = 1; break;
        }
        assert(dds_io_init() == -EACCES && !owned);
        assert(rcc.AHB1ENR == 0 && rcc.AHB2ENR == 0 && rcc.APB1ENR == 0);
    }
}

static void start_stop(void)
{
    start();
    assert(hw_timer.ARR == 999 && hw_timer.PSC == 0 && (hw_timer.CR1 & TIM_CR1_CEN));
    assert(channel.CNDTR == DDS_BUFFER_SAMPLES);
    assert(hw_dac.DHR12R1 == samples[DDS_BUFFER_SAMPLES - 1]);
    assert((hw_dac.CR & (DAC_CR_EN1 | DAC_CR_TEN1 | DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1)) ==
        (DAC_CR_EN1 | DAC_CR_TEN1 | DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1));
    assert(dds_io_check() == 0);
    assert(dds_io_stop() == 0);
    assert_quiet(DDS_FAULT_NONE);
    assert(hw_dac.CR & DAC_CR_EN1);
    assert(dds_io_check() == 0 && dds_io_stop() == 0 && dds_io_start() == 0);
}

static void frequency_restart(void)
{
    const uint32_t hz[] = {2000, 5000, 8000, 10000};
    assert(dds_io_init() == 0);
    for (unsigned int i = 0; i < 4; ++i) {
        assert(dds_io_configure(hz[i], 70) == 0);
        assert(dds_io_start() == 0 && snapshot().frequency_hz == hz[i]);
        assert((hw_timer.ARR + 1u) * DDS_CYCLE_SAMPLES * hz[i] == 170000000u);
        channel.CNDTR = 23;
        assert(dds_io_stop() == 0 && dds_io_check() == 0);
    }
    assert(dds_io_start() == 0 && channel.CNDTR == DDS_BUFFER_SAMPLES);
}

static void assert_latched(enum dds_fault expected)
{
    assert_quiet(expected);
    assert(!snapshot().ready && dds_io_start() == -EIO);
    assert(dds_io_configure(5000, 70) == -EIO && dds_io_init() == -EBUSY);
    dds_io_fault_stop();
    assert(snapshot().fault == expected);
}

static void dma_fault(void)
{
    start(); primask = 1; hw_dma.ISR |= 8u;
    handlers[DMA1_Channel1_IRQn](NULL);
    assert(primask == 1);
    assert_latched(DDS_FAULT_DMA);
    reset(); start(); hw_dma.ISR |= 8u;
    assert(dds_io_check() == -EIO);
    assert_latched(DDS_FAULT_DMA);
}

static void dac_fault(void)
{
    start(); hw_dac.SR |= 0x2000u;
    handlers[TIM6_DAC_IRQn](NULL);
    assert(primask == 0 && (hw_dac.SR & 0x2000u) == 0);
    assert_latched(DDS_FAULT_DAC);
    reset(); start(); hw_dac.SR |= 0x2000u;
    assert(dds_io_check() == -EIO);
    assert_latched(DDS_FAULT_DAC);
}

static void startup_error(void)
{
    assert(dds_io_init() == 0 && dds_io_configure(2000, 70) == 0);
    dac_ready = false;
    assert(dds_io_start() == -EIO);
    assert_latched(DDS_FAULT_DAC);
    reset(); assert(dds_io_init() == 0 && dds_io_configure(2000, 70) == 0);
    settle = false;
    assert(dds_io_start() == -EIO && !(hw_dac.CR & DAC_CR_EN1));
    assert_latched(DDS_FAULT_DAC);
}

static void stop_timeout(void)
{
    start(); settle = false; hw_dac.DOR1 = 2100;
    assert(dds_io_stop() == -EIO && !(hw_dac.CR & DAC_CR_EN1));
    assert_latched(DDS_FAULT_STOP);
    reset(); start(); dma_stuck = true;
    assert(dds_io_stop() == -EIO && !(hw_dac.CR & DAC_CR_EN1));
    assert_latched(DDS_FAULT_STOP);
}

static void resource_loss(void)
{
    for (unsigned int i = 0; i < 10; ++i) {
        reset(); start();
        switch (i) {
        case 0: channel.CMAR ^= 4; break;
        case 1: channel.CPAR ^= 4; break;
        case 2: channel.CNDTR = DDS_BUFFER_SAMPLES + 1; break;
        case 3: mux.CCR ^= 1; break;
        case 4: hw_dac.MCR = 0; break;
        case 5: hw_timer.PSC = 1; break;
        case 6: hw_timer.DIER = 1; break;
        case 7: vref_ready = false; break;
        case 8: rcc.AHB2ENR |= LL_AHB2_GRP1_PERIPH_DAC3; break;
        case 9: pin_mode = 1; break;
        }
        assert(dds_io_check() == -EIO);
        assert_latched(DDS_FAULT_RESOURCE);
    }
}

static void clock_loss(void)
{
    start(); rcc.AHB1ENR &= ~LL_AHB1_GRP1_PERIPH_DMAMUX1;
    assert(dds_io_check() == -EIO && !(hw_dac.CR & DAC_CR_EN1));
    assert_latched(DDS_FAULT_RESOURCE);
    reset(); start(); rcc.APB1ENR = 0;
    assert(dds_io_stop() == -EIO && !(hw_dac.CR & DAC_CR_EN1));
    assert(!snapshot().running && snapshot().fault == DDS_FAULT_STOP);
    assert(!(channel.CCR & DMA_CCR_EN));
}

static void external_fault(void)
{
    dds_io_fault_stop();
    assert(snapshot().fault == DDS_FAULT_EXTERNAL && dds_io_init() == -EBUSY);
    reset(); start(); dds_io_fault_stop();
    assert_latched(DDS_FAULT_EXTERNAL);
}

static void startup_cancel(void)
{
    assert(dds_io_init() == 0 && dds_io_configure(2000, 70) == 0);
    wait_hook = dds_io_fault_stop;
    assert(dds_io_start() == -EIO);
    assert_latched(DDS_FAULT_EXTERNAL);
}

static void adc_unchanged(void)
{
    for (unsigned int i = 0; i < 32; ++i) { adc_registers[i] = 0xabc000u + i; }
    rcc.AHB2ENR = LL_AHB2_GRP1_PERIPH_ADC12;
    uint32_t before[32]; memcpy(before, adc_registers, sizeof(before));
    start(); assert(dds_io_stop() == 0 && dds_io_start() == 0); dds_io_fault_stop();
    assert(memcmp(before, adc_registers, sizeof(before)) == 0);
    assert(rcc.AHB2ENR & LL_AHB2_GRP1_PERIPH_ADC12);
    assert(vref.CSR == VREFBUF_CSR_ENVR && vref_scale == LL_VREFBUF_VOLTAGE_SCALE2);
}

#if defined(CONFIG_HT_OUTPUT)
static void sample_only(void)
{
    const uint32_t hz[] = {2000, 5000, 8000, 10000};
    assert(dds_io_sample_start(2000) == -EACCES);
    assert(dds_io_init() == 0);
    assert(dds_io_sample_start(2001) == -EINVAL && !sampling);
    for (size_t i = 0; i < sizeof(hz) / sizeof(hz[0]); ++i) {
        unsigned int before = software_triggers;
        primask = 1;
        assert(dds_io_sample_start(hz[i]) == 0 && primask == 1);
        assert(sampling && !snapshot().running && snapshot().ready);
        assert(software_triggers == before);
        assert(hw_timer.CR1 == (TIM_CR1_ARPE | TIM_CR1_CEN));
        assert(hw_timer.CR2 == LL_TIM_TRGO_UPDATE);
        assert((hw_timer.ARR + 1u) * DDS_CYCLE_SAMPLES * hz[i] == SystemCoreClock);
        assert(!(hw_dac.CR & (DAC_CR_EN1 | DAC_CR_TEN1 | DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1)));
        assert(!(channel.CCR & (DMA_CCR_EN | DMA_IRQS)));
        assert(dds_io_check() == 0);
        assert(dds_io_sample_start(hz[i]) == -EBUSY);
        assert(dds_io_configure(2000, 70) == -EBUSY && dds_io_start() == -EBUSY);
        assert(dds_io_set_amplitude(70) == -EACCES);
        assert(dds_io_stop() == 0 && !sampling && primask == 1);
        assert_quiet(DDS_FAULT_NONE);
        assert(dds_io_check() == 0);
    }
}

static void sample_restart(void)
{
    start();
    assert(dds_io_sample_start(5000) == -EBUSY && snapshot().running);
    assert(dds_io_stop() == 0);
    unsigned int before = software_triggers;
    assert(dds_io_sample_start(5000) == 0);
    assert(software_triggers == before && !snapshot().running && sampling);
    assert((hw_dac.CR & DAC_CR_EN1) && hw_dac.DOR1 == DDS_MIDPOINT);
    assert(!(hw_dac.CR & (DAC_CR_TEN1 | DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1)));
    assert(!(channel.CCR & (DMA_CCR_EN | DMA_IRQS)));
    assert(dds_io_check() == 0 && dds_io_stop() == 0);
    assert(dds_io_start() == -EACCES);
    assert(dds_io_configure(10000, 90) == 0 && dds_io_start() == 0);
    assert(!sampling && snapshot().running && snapshot().frequency_hz == 10000);
    assert((hw_timer.ARR + 1u) * DDS_CYCLE_SAMPLES * 10000u == SystemCoreClock);
    assert(dds_io_check() == 0);
}

static void sample_fault(void)
{
    for (unsigned int i = 0; i < 3; ++i) {
        reset(); assert(dds_io_init() == 0 && dds_io_sample_start(2000) == 0);
        enum dds_fault expected;
        if (i == 0) { dds_io_fault_stop(); expected = DDS_FAULT_EXTERNAL; }
        else if (i == 1) { hw_dma.ISR |= 8; handlers[0](NULL); expected = DDS_FAULT_DMA; }
        else { hw_dac.SR |= 0x2000u; handlers[1](NULL); expected = DDS_FAULT_DAC; }
        assert(!sampling);
        assert_latched(expected);
        assert(dds_io_sample_start(2000) == -EIO);
    }
}

static void sample_resource(void)
{
    for (unsigned int i = 0; i < 8; ++i) {
        reset(); assert(dds_io_init() == 0 && dds_io_sample_start(2000) == 0);
        switch (i) {
        case 0: hw_dac.CR |= DAC_CR_TEN1; break;
        case 1: hw_dac.CR |= DAC_CR_DMAEN1; break;
        case 2: channel.CCR |= DMA_CCR_EN; break;
        case 3: hw_timer.CR1 &= ~TIM_CR1_CEN; break;
        case 4: hw_timer.CR2 = LL_TIM_TRGO_ENABLE; break;
        case 5: hw_timer.ARR++; break;
        case 6: vref_ready = false; break;
        case 7: rcc.AHB1ENR &= ~LL_AHB1_GRP1_PERIPH_DMAMUX1; break;
        }
        assert(dds_io_check() == -EIO && !sampling);
        assert_latched(DDS_FAULT_RESOURCE);
        assert(dds_io_sample_start(2000) == -EIO);
    }
}

static void dynamic_amplitude(void)
{
    assert(dds_io_set_amplitude(1) == -EACCES);
    assert(dds_io_init() == 0 && dds_io_configure(2000, 0) == 0 && dds_io_start() == 0);
    assert(dds_io_set_amplitude(2048) == -EINVAL);
    assert(dds_io_set_amplitude(500) == 0 && snapshot().amplitude == 0);
    channel.CNDTR = 840; hw_dma.ISR = 4;
    handlers[DMA1_Channel1_IRQn](NULL);
    assert(snapshot().running && snapshot().amplitude == 0);
    for (size_t i = 850; i < 1700; ++i) assert(samples[i] == DDS_MIDPOINT);
    uint16_t expected[850]; assert(dds_wave(expected, 850, 500) == 0);
    assert(memcmp(samples, expected, sizeof(expected)) == 0);
    channel.CNDTR = 1690; hw_dma.ISR = 2;
    handlers[DMA1_Channel1_IRQn](NULL);
    assert(snapshot().amplitude == 500 && memcmp(samples + 850, expected, sizeof(expected)) == 0);
    assert(dds_io_set_amplitude(0) == 0);
    channel.CNDTR = 840; hw_dma.ISR = 4; handlers[0](NULL);
    channel.CNDTR = 1690; hw_dma.ISR = 2; handlers[0](NULL);
    assert(snapshot().amplitude == 0);
    for (size_t i = 0; i < 1700; ++i) assert(samples[i] == DDS_MIDPOINT);
    assert(dds_io_stop() == 0 && !(channel.CCR & (DMA_CCR_HTIE | DMA_CCR_TCIE)));
    assert(dds_io_set_amplitude(100) == -EACCES);
}

static void cross_half(void) { channel.CNDTR = 1600; hw_dma.ISR |= 2; }
static void dynamic_deadline(void)
{
    start(); hw_dma.ISR = 6; handlers[0](NULL); assert_latched(DDS_FAULT_DMA);
    reset(); start(); hw_dma.ISR = 4; channel.CNDTR = 1600;
    handlers[0](NULL); assert_latched(DDS_FAULT_DMA);
    reset(); start(); assert(dds_io_set_amplitude(100) == 0);
    hw_dma.ISR = 4; channel.CNDTR = 800; barrier_hook = cross_half;
    handlers[0](NULL); assert_latched(DDS_FAULT_DMA);
}

static void dynamic_fault(void)
{
    start(); assert(dds_io_set_amplitude(100) == 0);
    hw_dma.ISR = 4; channel.CNDTR = 800; barrier_hook = dds_io_fault_stop;
    handlers[0](NULL); assert_latched(DDS_FAULT_EXTERNAL);
    assert(dds_io_set_amplitude(200) == -EIO);
}
#endif

int main(int argc, char **argv)
{
    assert(argc == 2); reset();
#define RUN(name) if (strcmp(argv[1], #name) == 0) { name(); puts("DDS IO test passed"); return 0; }
    RUN(init_idle) RUN(input_guards) RUN(resource_conflicts) RUN(reference_guards)
    RUN(start_stop) RUN(frequency_restart) RUN(dma_fault) RUN(dac_fault)
    RUN(startup_error) RUN(stop_timeout) RUN(resource_loss) RUN(clock_loss)
    RUN(external_fault) RUN(startup_cancel) RUN(adc_unchanged)
#if defined(CONFIG_HT_OUTPUT)
    RUN(dynamic_amplitude) RUN(dynamic_deadline) RUN(dynamic_fault)
    RUN(sample_only) RUN(sample_restart) RUN(sample_fault) RUN(sample_resource)
#endif
    assert(!"unknown test"); return 1;
}
