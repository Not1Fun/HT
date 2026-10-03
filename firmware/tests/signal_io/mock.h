/* @brief ADC寄存器与时钟/DMA队列边界模拟；生产采样与RMS代码直接参与测试。 */
#ifndef HT_SIGNAL_IO_MOCK_H
#define HT_SIGNAL_IO_MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ARG_UNUSED(x) (void)(x)
#define BUILD_ASSERT(c, ...) _Static_assert(c, #c)
#define DT_NODELABEL(x) 0
#define DT_NODE_HAS_STATUS(n, s) 0
#define __aligned(n) __attribute__((aligned(n)))
static void (*barrier_hook)(void);
#define __DMB() do { if (barrier_hook) barrier_hook(); } while (0)
typedef unsigned int atomic_t;
static inline bool atomic_test_and_set_bit(atomic_t *v, unsigned int bit)
{ bool before = (*v & BIT(bit)) != 0; *v |= BIT(bit); return before; }
static inline bool atomic_test_bit(const atomic_t *v, unsigned int bit) { return (*v & BIT(bit)) != 0; }
static inline void atomic_clear_bit(atomic_t *v, unsigned int bit) { *v &= ~BIT(bit); }
static inline void atomic_clear(atomic_t *v) { *v = 0; }
#define K_NO_WAIT 0
struct k_msgq { uint8_t *data; size_t size; unsigned int count; };
#define K_MSGQ_DEFINE(n, size, depth, align) \
    static uint8_t n##_data[(size) * (depth)]; \
    static struct k_msgq n = {n##_data, size, 0}
static inline void k_msgq_purge(struct k_msgq *q) { q->count = 0; }
static inline int k_msgq_put(struct k_msgq *q, const void *p, int wait)
{
    (void)wait;
    assert(q->size <= sizeof(uint32_t));
    if (q->count == 3) return -1;
    memcpy(q->data + q->count++ * q->size, p, q->size); return 0;
}
static inline int k_msgq_get(struct k_msgq *q, void *p, int wait)
{
    (void)wait;
    assert(q->size <= sizeof(uint32_t));
    if (!q->count) return -1;
    memcpy(p, q->data, q->size);
    memmove(q->data, q->data + q->size, --q->count * q->size); return 0;
}
static int64_t now;
static uint64_t cycles;
static inline int64_t k_uptime_get(void) { return now; }
static inline void k_busy_wait(unsigned int us) { cycles += us; ++now; }
static inline uint64_t k_cycle_get_64(void) { return cycles; }
static inline uint64_t k_us_to_cyc_ceil64(unsigned int us) { return us; }
static inline uint32_t irq_lock(void) { return 0; }
static inline void irq_unlock(uint32_t key) { (void)key; }
#define DMA1_Channel2_IRQn 0
#define ADC1_2_IRQn 1
static bool enabled[2];
static void (*handlers[2])(const void *);
#define IRQ_CONNECT(n, p, h, c, f) (handlers[n] = h)
static inline void irq_enable(int n) { enabled[n] = true; }
static inline void irq_disable(int n) { enabled[n] = false; }
static inline void NVIC_ClearPendingIRQ(int n) { (void)n; }
#define DMA_CCR_EN 1u
#define DMA_CCR_TCIE 2u
#define DMA_CCR_HTIE 4u
#define DMA_CCR_TEIE 8u
#define DMA_CCR_CIRC 32u
#define DMA_CCR_MINC 128u
#define DMA_CCR_PSIZE_1 512u
#define DMA_CCR_MSIZE_1 2048u
#define DMA_CCR_PL_1 8192u
#define DMA_IFCR_CGIF2 0xf0u
#define LL_DMAMUX_REQ_ADC1 5u
#define LL_AHB2_GRP1_PERIPH_ADC12 0x2000u
#define ADC_CR_ADEN 1u
#define ADC_CR_ADSTART 4u
#define ADC_CR_JADSTART 8u
#define ADC_CR_ADSTP 16u
#define ADC_CR_ADCAL (1u << 31)
#define ADC_IER_OVRIE 16u
#define ADC_ISR_OVR 16u
#define ADC_ISR_ADRDY 1u
#define ADC_ISR_JEOS 64u
#define ADC_CFGR_JQDIS (1u << 31)
#define ADC_CFGR_OVRMOD (1u << 12)
#define LL_ADC_REG_OVR_DATA_OVERWRITTEN ADC_CFGR_OVRMOD
#define LL_ADC_REG_RANK_1 1u
#define LL_ADC_INJ_RANK_1 1u
#define LL_ADC_CHANNEL_1 1u
#define LL_ADC_CHANNEL_2 2u
#define LL_ADC_CHANNEL_3 3u
#define LL_ADC_CHANNEL_4 4u
#define LL_ADC_CHANNEL_6 6u
#define LL_ADC_CHANNEL_7 7u
#define LL_ADC_CHANNEL_9 9u
#define __LL_ADC_CHANNEL_TO_DECIMAL_NB(c) (c)
#define LL_ADC_DELAY_INTERNAL_REGUL_STAB_US 20u
#define LL_ADC_RESOLUTION_12B 0u
#define LL_ADC_DATA_ALIGN_RIGHT 0u
#define LL_ADC_DIFFERENTIAL_ENDED 1u
#define LL_ADC_SINGLE_ENDED 0u
#define LL_ADC_SAMPLINGTIME_24CYCLES_5 3u
#define LL_ADC_SAMPLINGTIME_640CYCLES_5 7u
#define LL_ADC_REG_SEQ_SCAN_DISABLE 0u
#define LL_ADC_REG_CONV_SINGLE 0u
#define LL_ADC_CLOCK_SYNC_PCLK_DIV4 3u
#define LL_ADC_MULTI_DUAL_REG_SIMULT 6u
#define LL_ADC_MULTI_REG_DMA_UNLMT_RES12_10B 2u
#define LL_ADC_REG_TRIG_EXT_TIM6_TRGO 13u
#define LL_ADC_REG_TRIG_EXT_RISING 1u
#define LL_ADC_REG_DMA_TRANSFER_UNLIMITED 3u
#define LL_GPIO_PIN_0 1u
#define LL_GPIO_PIN_1 2u
#define LL_GPIO_PIN_3 8u
#define LL_GPIO_PIN_6 64u
#define LL_GPIO_PIN_7 128u
#define LL_GPIO_MODE_ANALOG 3u
#define LL_GPIO_PULL_NO 0u
#define GPIOA ((GPIO_TypeDef *)0)
#define GPIOC ((GPIO_TypeDef *)1)
typedef struct { uint32_t CR, CFGR, DIFSEL, IER, ISR, JSQR, channel, sampling[19]; } ADC_TypeDef;
typedef int GPIO_TypeDef;
static ADC_TypeDef adc[2];
static struct { uint32_t CCR, CDR; } common;
static struct { uint32_t CCR, CNDTR, CPAR, CMAR; } hw_dma;
static struct { uint32_t CCR; } mux;
static struct { uint32_t IFCR; } dma_flags;
static uint32_t adc_clock, irq_flags;
static bool stop_stuck;
static uintptr_t sram_begin, sram_end;
#define SRAM1_BASE sram_begin
#define SRAM2_BASE sram_end
#define SRAM2_SIZE 0u
#define ADC1 (&adc[0])
#define ADC2 (&adc[1])
#define ADC12_COMMON (&common)
#define DMA1_Channel2 (&hw_dma)
#define DMAMUX1_Channel1 (&mux)
#define DMA1 (&dma_flags)
static inline bool LL_AHB2_GRP1_IsEnabledClock(uint32_t n) { return (adc_clock & n) == n; }
static inline void LL_AHB2_GRP1_EnableClock(uint32_t n) { adc_clock |= n; }
static inline void LL_AHB2_GRP1_ForceReset(uint32_t n) { (void)n; memset(adc, 0, sizeof(adc)); }
static inline void LL_AHB2_GRP1_ReleaseReset(uint32_t n) { (void)n; }
static inline uint32_t LL_DBGMCU_GetRevisionID(void) { return 0x2003; }
static inline uint32_t LL_GPIO_GetPinMode(GPIO_TypeDef *p, uint32_t n) { (void)p; (void)n; return LL_GPIO_MODE_ANALOG; }
static inline uint32_t LL_GPIO_GetPinPull(GPIO_TypeDef *p, uint32_t n) { (void)p; (void)n; return LL_GPIO_PULL_NO; }
static inline bool LL_ADC_REG_IsConversionOngoing(ADC_TypeDef *a) { return (a->CR & ADC_CR_ADSTART) != 0; }
static inline void LL_ADC_REG_StopConversion(ADC_TypeDef *a)
{ (void)a; if (!stop_stuck) { adc[0].CR &= ~ADC_CR_ADSTART; adc[1].CR &= ~ADC_CR_ADSTART; } }
static inline void LL_ADC_REG_StartConversion(ADC_TypeDef *a)
{ (void)a; adc[0].CR |= ADC_CR_ADSTART; adc[1].CR |= ADC_CR_ADSTART; }
static inline uint32_t LL_ADC_REG_GetSequencerRanks(ADC_TypeDef *a, uint32_t n) { (void)n; return a->channel; }
static inline void LL_ADC_REG_SetSequencerRanks(ADC_TypeDef *a, uint32_t n, uint32_t c) { (void)n; a->channel = c; }
static inline void LL_ADC_SetChannelSingleDiff(ADC_TypeDef *a, uint32_t c, uint32_t m) { (void)m; a->DIFSEL |= BIT(c); }
static inline void LL_ADC_ClearFlag_OVR(ADC_TypeDef *a) { a->ISR &= ~ADC_ISR_OVR; }
static inline void LL_ADC_ClearFlag_EOC(ADC_TypeDef *a) { (void)a; }
static inline void LL_ADC_ClearFlag_EOS(ADC_TypeDef *a) { (void)a; }
static inline void LL_ADC_ClearFlag_ADRDY(ADC_TypeDef *a) { a->ISR &= ~ADC_ISR_ADRDY; }
static inline void LL_ADC_ClearFlag_JEOS(ADC_TypeDef *a) { a->ISR &= ~ADC_ISR_JEOS; }
static inline void LL_ADC_Enable(ADC_TypeDef *a) { a->CR |= ADC_CR_ADEN; a->ISR |= ADC_ISR_ADRDY; }
static inline void LL_ADC_DisableDeepPowerDown(ADC_TypeDef *a) { (void)a; }
static inline void LL_ADC_EnableInternalRegulator(ADC_TypeDef *a) { (void)a; }
static inline void LL_ADC_StartCalibration(ADC_TypeDef *a, uint32_t m) { (void)a; (void)m; }
static inline void LL_ADC_SetChannelSamplingTime(ADC_TypeDef *a, uint32_t c, uint32_t t)
{ assert(c < ARRAY_SIZE(a->sampling)); a->sampling[c] = t; }
static inline void LL_ADC_INJ_SetSequencerRanks(ADC_TypeDef *a, uint32_t n, uint32_t c) { (void)n; a->JSQR = c; }
static inline void LL_ADC_INJ_StartConversion(ADC_TypeDef *a) { a->ISR |= ADC_ISR_JEOS; }
static inline uint16_t LL_ADC_INJ_ReadConversionData12(ADC_TypeDef *a, uint32_t n) { (void)a; (void)n; return 1400; }
#define LL_ADC_SetResolution(a, v) ((void)(a), (void)(v))
#define LL_ADC_SetDataAlignment(a, v) ((void)(a), (void)(v))
#define LL_ADC_REG_SetSequencerLength(a, v) ((void)(a), (void)(v))
#define LL_ADC_REG_SetContinuousMode(a, v) ((void)(a), (void)(v))
#define LL_ADC_REG_SetOverrun(a, v) ((a)->CFGR = ((a)->CFGR & ~ADC_CFGR_OVRMOD) | (v))
#define LL_ADC_SetCommonClock(a, v) ((a)->CCR |= (v))
#define LL_ADC_SetMultimode(a, v) ((a)->CCR |= (v) << 4)
#define LL_ADC_SetMultiDMATransfer(a, v) ((a)->CCR |= (v) << 8)
#define LL_ADC_REG_SetTriggerSource(a, v) ((a)->CFGR |= (v) << 5)
#define LL_ADC_REG_SetTriggerEdge(a, v) ((a)->CFGR |= (v) << 10)
#define LL_ADC_REG_SetDMATransfer(a, v) ((a)->CFGR |= (v))
#define LL_DMA_IsActiveFlag_HT2(d) ((void)(d), (irq_flags & 1u) != 0)
#define LL_DMA_IsActiveFlag_TC2(d) ((void)(d), (irq_flags & 2u) != 0)
#define LL_DMA_IsActiveFlag_TE2(d) ((void)(d), (irq_flags & 4u) != 0)
#define LL_DMA_ClearFlag_HT2(d) ((void)(d), irq_flags &= ~1u)
#define LL_DMA_ClearFlag_TC2(d) ((void)(d), irq_flags &= ~2u)
#endif
