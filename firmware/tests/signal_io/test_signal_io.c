/* @brief 验证生产ADC采样在待机、输出、温度维护和故障之间的切换。 */
#include "mock.h"
#include "../../src/platform/signal_io.c"
#include <stdio.h>
#include <stdlib.h>

static bool clock_on, waveform, clock_fault, fail_start;
static unsigned int sample_starts, wave_starts, stops;
static uint32_t configured_frequency;
static bool recover_fault;
int dds_io_recover(void) {
    assert(!clock_on && !waveform);
    clock_fault=false;
    if (recover_fault) signal_io_fault();
    return 0;
}
int board_io_start_reference(void) { return 0; }
int dds_io_init(void) { return 0; }
int dds_io_check(void) { return clock_fault ? -EIO : 0; }
void dds_io_fault_stop(void) { clock_fault = true; clock_on = waveform = false; }
int dds_io_stop(void) { ++stops; clock_on = waveform = false; return 0; }
int dds_io_configure(uint32_t frequency_hz, uint16_t amplitude)
{
    assert(!clock_on && !waveform && amplitude == 0);
    assert(!(adc[0].CR & ADC_CR_ADSTART) && !(hw_dma.CCR & DMA_CCR_EN));
    configured_frequency = frequency_hz;
    return clock_fault ? -EIO : 0;
}
int dds_io_start(void)
{
    assert(!clock_on && (adc[0].CR & ADC_CR_ADSTART) && (hw_dma.CCR & DMA_CCR_EN));
    if (fail_start) { signal_io_fault(); return -EIO; }
    clock_on = waveform = true; ++wave_starts; return 0;
}
int dds_io_sample_start(uint32_t frequency_hz)
{
    assert(frequency_hz == 2000 && !clock_on && !waveform);
    assert((adc[0].CR & ADC_CR_ADSTART) && (hw_dma.CCR & DMA_CCR_EN));
    if (clock_fault) return -EIO;
    clock_on = true; ++sample_starts; return 0;
}

static struct signal_snapshot current;
static void init(void)
{
    sram_begin = (uintptr_t)buffer; sram_end = sram_begin + sizeof(buffer);
    assert(signal_io_init() == 0);
    assert(ready && active && !output && clock_on && !waveform);
    assert(sample_starts == 1 && wave_starts == 0 && !failed);
    assert((adc[0].CFGR & ADC_CFGR_OVRMOD) && (adc[1].CFGR & ADC_CFGR_OVRMOD));
    assert(adc[0].IER == ADC_IER_OVRIE && adc[1].IER == ADC_IER_OVRIE);
    assert(adc[0].sampling[6] == LL_ADC_SAMPLINGTIME_24CYCLES_5);
    assert(adc[0].sampling[7] == LL_ADC_SAMPLINGTIME_24CYCLES_5);
    assert(adc[1].sampling[3] == LL_ADC_SAMPLINGTIME_24CYCLES_5);
    assert(adc[1].sampling[4] == LL_ADC_SAMPLINGTIME_24CYCLES_5);
    assert(adc[0].sampling[9] == LL_ADC_SAMPLINGTIME_640CYCLES_5);
    assert(adc[0].sampling[2] == LL_ADC_SAMPLINGTIME_640CYCLES_5);
    assert(adc[0].sampling[1] == LL_ADC_SAMPLINGTIME_640CYCLES_5);
    assert(signal_io_poll(&current) == 0);
    assert(!current.reading.valid);
}

static void fill(unsigned int offset, unsigned int voltage, unsigned int current_code)
{
    for (unsigned int n = 0; n < HALF * 2; ++n) {
        int sign = n & 1u ? 1 : -1;
        uint16_t v = (uint16_t)((int)offset + sign * (int)voltage);
        uint16_t i = (uint16_t)((int)offset + sign * (int)current_code);
        buffer[n] = v | ((uint32_t)i << 16);
    }
}

static void deliver_block(void)
{
    assert(clock_on && enabled[0] && (hw_dma.CCR & DMA_CCR_EN));
    bool first = number % 2 == 0;
    irq_flags = first ? 1 : 2;
    hw_dma.CNDTR = first ? HALF : HALF * 2;
    handlers[0](NULL);
}

static int half_block(void)
{
    deliver_block();
    now += 5;
    return signal_io_poll(&current);
}

static void window(void)
{
    for (unsigned int n = 0; n < 11; ++n) assert(half_block() == 0);
    assert(current.reading.valid);
}

static void idle_reading(void)
{
    init(); fill(2048, 0, 0); window();
    assert(current.reading.voltage_mv == 0 && current.reading.current_ma == 0);
    uint32_t first = current.reading.sequence;
    fill(2048, 2, 4); window();
    assert(current.reading.sequence > first && now - current.reading.time_ms <= 5);
    assert(current.reading.voltage_mv == 629 && current.reading.current_ma == 35);
    assert(!waveform && wave_starts == 0 && sample_starts == 1);
}

static void output_transition(void)
{
    init(); fill(2048, 0, 0); window();
    assert(signal_io_start(10000) == 0);
    assert(waveform && output && active && configured_frequency == 10000);
    assert(!value.reading.valid && accumulator.count == 0 && blocks.count == 0);
    assert(signal_io_start(5000) == -EACCES);
    assert(half_block() == 0 && !current.reading.valid && accumulator.count == 0);
    window();
    assert(signal_io_stop() == 0);
    assert(!waveform && !output && active && clock_on && sample_starts == 2);
    assert(!value.reading.valid && accumulator.count == 0 && blocks.count == 0);
    assert(half_block() == 0 && !current.reading.valid && accumulator.count == 0);
    window();
    assert(wave_starts == 1);
}

static void temperature_resume(void)
{
    init(); fill(2048, 0, 0);
    for (unsigned int n = 0; n < 4; ++n) {
        window();
        now += 200;
        assert(signal_io_poll(&current) == 0);
        assert(current.temperature_ms == now && accumulator.count == 0 && discard);
        for (size_t i = 0; i < 3; ++i) assert(current.temperature.samples[i].status == NTC_OK);
        assert(clock_on && !waveform && (hw_dma.CCR & DMA_CCR_EN));
    }
    assert(sample_starts == 1 && wave_starts == 0);
}

static void sample_fault(void)
{
    init(); assert(signal_io_start(2000) == 0); fill(0, 0, 0);
    assert(half_block() == 0);
    assert(half_block() == -ERANGE);
    assert(failed && !clock_on && !active && !(hw_dma.CCR & DMA_CCR_EN));
    assert(signal_io_stop() == 0 && sample_starts == 1 && !clock_on);
    assert(signal_io_start(2000) == -EACCES);
}

static void idle_invalid(void)
{
    init(); fill(2048, 2, 4); window();
    for (unsigned int n = 0; n < HALF * 2; ++n) buffer[n] = (buffer[n] & 0xffffu) | (3107u << 16);
    for (unsigned int n = 0; n < 11; ++n) assert(half_block() == 0);
    assert(!current.reading.valid && !failed && active && !waveform);
    now += 200;
    assert(signal_io_poll(&current) == 0 && current.temperature_ms == now);
    fill(0, 0, 0);
    for (unsigned int n = 0; n < 4; ++n) assert(half_block() == 0);
    assert(!current.reading.valid && !failed && active);
    fill(2048, 2, 4); window();
    assert(current.reading.valid && current.reading.current_ma == 35 && wave_starts == 0);
}

static void stop_fault(void)
{
    init(); stop_stuck = true;
    assert(signal_io_stop() == -ETIMEDOUT);
    assert(failed && !clock_on && !active && sample_starts == 1);
}

static void stale_window(void)
{
    init(); fill(2048, 2, 4);
    for (unsigned int n = 0; n < 6; ++n) assert(half_block() == 0);
    assert(accumulator.count == 5 * HALF && !current.reading.valid);
    assert(signal_io_start(5000) == 0);
    fill(2048, 0, 0); window();
    assert(current.reading.voltage_mv == 0 && current.reading.current_ma == 0);
    assert(current.reading.sequence == 1);
}

static void dma_fault(void)
{
    init(); irq_flags = 3; handlers[0](NULL);
    assert(failed && !clock_on && !waveform && !(hw_dma.CCR & DMA_CCR_EN));
    assert(signal_io_stop() == 0 && sample_starts == 1);
}

static void start_fault(void)
{
    init(); fail_start = true;
    assert(signal_io_start(2000) == -EIO);
    assert(failed && !output && !clock_on && !active);
    assert(signal_io_stop() == 0 && sample_starts == 1);
}

static void adc_overrun(void)
{
    init(); adc[1].ISR |= ADC_ISR_OVR; handlers[1](NULL);
    assert(failed && !clock_on && !active && !(hw_dma.CCR & DMA_CCR_EN));
    assert(signal_io_stop() == 0 && sample_starts == 1);
}

static void resource_loss(void)
{
    init(); adc[0].channel = 3;
    assert(signal_io_poll(&current) == -EIO);
    assert(failed && !clock_on && !active);
}

static void block_ownership(void)
{
    init(); fill(2048, 2, 4);
    for (unsigned int n = 0; n < 3; ++n) deliver_block();
    uint8_t held;
    assert(k_msgq_get(&blocks, &held, K_NO_WAIT) == 0 && held == 0);
    assert(atomic_test_bit(&busy, held));
    uint32_t original = pool[held].samples[0];
    deliver_block();
    assert(!failed && blocks.count == 3 && busy == 15);
    fill(2048, 0, 0); deliver_block();
    assert(failed && !clock_on && pool[held].samples[0] == original && number == 4);
}

static void queue_overflow(void)
{
    init(); fill(2048, 0, 0);
    for (unsigned int n = 0; n < 4; ++n) deliver_block();
    assert(failed && !clock_on && blocks.count == 3 && !atomic_test_bit(&busy, 3));
    assert(signal_io_stop() == 0 && blocks.count == 0 && busy == 0 && sample_starts == 1);
}

static void temperature_purge(void)
{
    init(); fill(2048, 2, 4);
    for (unsigned int n = 0; n < 3; ++n) deliver_block();
    assert(blocks.count == 3 && busy == 7);
    assert(temperature() == 0 && blocks.count == 0 && busy == 0 && expected == number);
    assert(discard && clock_on && active);
    window();
    assert(current.reading.current_ma == 35 && !failed && busy == 0);
}

static void overlap(void) { hw_dma.CNDTR = HALF * 2; irq_flags |= 2; }
static void dma_copy_overlap(void)
{
    init(); fill(2048, 0, 0); barrier_hook = overlap; deliver_block();
    assert(failed && !clock_on && busy == 0 && blocks.count == 0);
}

static void copy_fault(void)
{
    init(); fill(2048, 0, 0); barrier_hook = signal_io_fault; deliver_block();
    assert(failed && !clock_on && busy == 0 && blocks.count == 0);
    assert(signal_io_stop() == 0 && sample_starts == 1);
}

static void recover_reading(void)
{
    init(); fill(2048, 2, 4); window(); uint32_t seq = current.reading.sequence;
    assert(signal_io_start(2000) == 0 && signal_io_recover() == -EACCES);
    signal_io_fault(); assert(signal_io_recover() == 0);
    assert(!failed && active && !output && !waveform && wave_starts == 1);
    assert(signal_io_poll(&current) == 0 && !current.reading.valid);
    assert(current.temperature_ms == now);
    fill(2048, 0, 0); window();
    assert(current.reading.sequence > seq && current.reading.current_ma == 0);
    signal_io_fault(); recover_fault = true;
    assert(signal_io_recover() == -EIO && failed && !active && !clock_on);
    recover_fault = false; assert(signal_io_recover() == 0 && !failed);
}
static void recover_resources(void)
{
    init(); signal_io_fault(); adc[0].channel = 3;
    assert(signal_io_recover() == -EIO && failed && !clock_on && !waveform);
    adc[0].channel = 6; assert(signal_io_recover() == 0);
    signal_io_fault(); stop_stuck = true; adc[0].CR |= ADC_CR_ADSTART;
    assert(signal_io_recover() == -ETIMEDOUT && failed && !clock_on);
}
int main(int argc, char **argv)
{
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    assert(argc == 2);
    struct { const char *name; void (*run)(void); } cases[] = {
        {"recover_reading", recover_reading}, {"recover_resources", recover_resources},
        {"idle_reading", idle_reading}, {"output_transition", output_transition},
        {"temperature_resume", temperature_resume}, {"sample_fault", sample_fault},
        {"stop_fault", stop_fault}, {"stale_window", stale_window},
        {"dma_fault", dma_fault}, {"start_fault", start_fault}, {"resource_loss", resource_loss},
        {"adc_overrun", adc_overrun}, {"idle_invalid", idle_invalid},
        {"block_ownership", block_ownership}, {"queue_overflow", queue_overflow},
        {"temperature_purge", temperature_purge}, {"dma_copy_overlap", dma_copy_overlap},
        {"copy_fault", copy_fault}
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
        if (strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run(); puts("PASS"); return 0;
    }
    return 1;
}
