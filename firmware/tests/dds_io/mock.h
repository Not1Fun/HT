/* @brief 仅模拟DDS用到的寄存器副作用、时钟、IRQ与线程互斥。 */
#ifndef HT_DDS_IO_MOCK_H
#define HT_DDS_IO_MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define BUILD_ASSERT(condition, ...) _Static_assert(condition, #condition)
#define DT_NODELABEL(label) 0
#define DT_NODE_HAS_STATUS(node, status) 0
#define __aligned(value) __attribute__((aligned(value)))
#define K_MUTEX_DEFINE(name) int name
#define K_FOREVER (-1)
static inline void k_mutex_lock(int *lock, int timeout) { (void)timeout; assert(!*lock); *lock = 1; }
static inline void k_mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static void (*wait_hook)(void);
static inline void k_busy_wait(unsigned int usec) { (void)usec; if (wait_hook) { wait_hook(); } }
static uint32_t primask;
static inline uint32_t __get_PRIMASK(void) { return primask; }
static inline void __disable_irq(void) { primask = 1; }
static inline void __set_PRIMASK(uint32_t value) { primask = value; }
static inline void __DMB(void) { }
#define DMA1_Channel1_IRQn 0
#define TIM6_DAC_IRQn 1
static void (*handlers[2])(const void *);
static bool irq_enabled[2];
#define IRQ_CONNECT(number, priority, handler, context, flags) (handlers[number] = handler)
static inline void NVIC_ClearPendingIRQ(int irq) { (void)irq; }
static inline void irq_enable(int irq) { irq_enabled[irq] = true; }
#define DMA_CCR_EN 0x1u
#define DMA_CCR_TEIE 0x8u
#define DMA_CCR_DIR 0x10u
#define DMA_CCR_CIRC 0x20u
#define DMA_CCR_MINC 0x80u
#define DMA_CCR_PSIZE_0 0x100u
#define DMA_CCR_MSIZE_0 0x400u
#define DMA_CCR_PL_1 0x2000u
#define DMA_IFCR_CGIF1 1u
#define DAC_CR_EN1 1u
#define DAC_CR_TEN1 4u
#define DAC_CR_DMAEN1 0x1000u
#define DAC_CR_DMAUDRIE1 0x2000u
#define TIM_CR1_CEN 1u
#define TIM_CR1_ARPE 0x80u
#define TIM_EGR_UG 1u
#define LL_TIM_TRGO_UPDATE 0x20u
#define LL_DAC_TRIG_EXT_TIM6_TRGO 0x38u
#define LL_DAC_HIGH_FREQ_MODE_ABOVE_160MHZ 0x8000u
#define LL_DAC_CHANNEL_1 0u
#define LL_DAC_DELAY_STARTUP_VOLTAGE_SETTLING_US 8u
#define LL_DMAMUX_REQ_DAC1_CH1 6u
#define LL_AHB1_GRP1_PERIPH_DMA1 1u
#define LL_AHB1_GRP1_PERIPH_DMAMUX1 4u
#define LL_AHB2_GRP1_PERIPH_DAC1 0x10000u
#define LL_AHB2_GRP1_PERIPH_DAC3 0x40000u
#define LL_AHB2_GRP1_PERIPH_ADC12 0x2000u
#define LL_APB1_GRP1_PERIPH_TIM6 0x10u
#define VREFBUF_CSR_ENVR 1u
#define VREFBUF_CSR_HIZ 2u
#define LL_VREFBUF_VOLTAGE_SCALE2 2u
#define LL_RCC_SYSCLK_DIV_1 0u
#define LL_RCC_APB1_DIV_1 0u
#define LL_GPIO_PIN_4 16u
#define LL_GPIO_MODE_ANALOG 3u
#define LL_GPIO_PULL_NO 0u

typedef struct { uint32_t CR, MCR, DHR12R1, DOR1, SR; } DAC_TypeDef;
typedef struct { uint32_t CR1, CR2, DIER, PSC, ARR, EGR, SR, CNT; } TIM_TypeDef;
typedef struct { uint32_t CCR, CNDTR, CPAR, CMAR; } DMA_Channel_TypeDef;
static DAC_TypeDef hw_dac;
static TIM_TypeDef hw_timer;
static DMA_Channel_TypeDef channel;
static struct { uint32_t IFCR, ISR; } hw_dma;
static struct { uint32_t CCR; } mux;
static struct { uint32_t AHB1ENR, AHB2ENR, APB1ENR; } rcc;
static struct { uint32_t CSR; } vref;
static uint32_t adc_registers[32];
static bool settle = true, dac_ready = true, dma_stuck;
static DAC_TypeDef *mock_dac(void)
{
    if (settle && (hw_dac.CR & (DAC_CR_EN1 | DAC_CR_TEN1)) == DAC_CR_EN1) {
        hw_dac.DOR1 = hw_dac.DHR12R1;
    }
    return &hw_dac;
}
static DMA_Channel_TypeDef *mock_channel(void)
{
    if (dma_stuck) { channel.CCR |= DMA_CCR_EN; }
    return &channel;
}
#define DAC1 mock_dac()
#define TIM6 (&hw_timer)
#define DMA1 (&hw_dma)
#define DMA1_Channel1 mock_channel()
#define DMAMUX1_Channel0 (&mux)
#define RCC (&rcc)
#define VREFBUF (&vref)
#define GPIOA 0
static uint32_t SystemCoreClock = 170000000u;
static uint32_t ahb_prescaler, apb_prescaler, pin_mode = LL_GPIO_MODE_ANALOG, pin_pull;
static bool vref_ready = true;
static uint32_t vref_scale = LL_VREFBUF_VOLTAGE_SCALE2;
static uintptr_t sram_begin, sram_end;
#define SRAM1_BASE sram_begin
#define SRAM2_BASE sram_end
#define SRAM2_SIZE 0u
static inline bool LL_AHB1_GRP1_IsEnabledClock(uint32_t mask) { return (rcc.AHB1ENR & mask) == mask; }
static inline bool LL_AHB2_GRP1_IsEnabledClock(uint32_t mask) { return (rcc.AHB2ENR & mask) == mask; }
static inline bool LL_APB1_GRP1_IsEnabledClock(uint32_t mask) { return (rcc.APB1ENR & mask) == mask; }
static inline void LL_AHB1_GRP1_EnableClock(uint32_t mask) { rcc.AHB1ENR |= mask; }
static inline void LL_AHB2_GRP1_EnableClock(uint32_t mask) { rcc.AHB2ENR |= mask; }
static inline void LL_APB1_GRP1_EnableClock(uint32_t mask) { rcc.APB1ENR |= mask; }
static inline void LL_AHB1_GRP1_DisableClock(uint32_t mask) { rcc.AHB1ENR &= ~mask; }
static inline void LL_AHB2_GRP1_DisableClock(uint32_t mask) { rcc.AHB2ENR &= ~mask; }
static inline void LL_APB1_GRP1_DisableClock(uint32_t mask) { rcc.APB1ENR &= ~mask; }
static inline void LL_AHB2_GRP1_ForceReset(uint32_t mask) { assert(mask == LL_AHB2_GRP1_PERIPH_DAC1); memset(&hw_dac, 0, sizeof(hw_dac)); }
static inline void LL_AHB2_GRP1_ReleaseReset(uint32_t mask) { assert(mask == LL_AHB2_GRP1_PERIPH_DAC1); }
static inline void LL_APB1_GRP1_ForceReset(uint32_t mask) { assert(mask == LL_APB1_GRP1_PERIPH_TIM6); memset(&hw_timer, 0, sizeof(hw_timer)); }
static inline void LL_APB1_GRP1_ReleaseReset(uint32_t mask) { assert(mask == LL_APB1_GRP1_PERIPH_TIM6); }
static inline uint32_t LL_RCC_GetAHBPrescaler(void) { return ahb_prescaler; }
static inline uint32_t LL_RCC_GetAPB1Prescaler(void) { return apb_prescaler; }
static inline bool LL_VREFBUF_IsVREFReady(void) { return vref_ready; }
static inline uint32_t LL_VREFBUF_GetVoltageScaling(void) { return vref_scale; }
static inline uint32_t LL_GPIO_GetPinMode(int port, uint32_t pin) { (void)port; assert(pin == LL_GPIO_PIN_4); return pin_mode; }
static inline uint32_t LL_GPIO_GetPinPull(int port, uint32_t pin) { (void)port; assert(pin == LL_GPIO_PIN_4); return pin_pull; }
static inline void LL_DAC_Enable(DAC_TypeDef *reg, uint32_t ch) { assert(ch == 0); reg->CR |= DAC_CR_EN1; }
static inline void LL_DAC_Disable(DAC_TypeDef *reg, uint32_t ch) { assert(ch == 0); reg->CR &= ~DAC_CR_EN1; }
static inline bool LL_DAC_IsReady(DAC_TypeDef *reg, uint32_t ch) { (void)reg; assert(ch == 0); return dac_ready; }
#define LL_DMA_IsActiveFlag_TE1(reg) (((reg)->ISR & 8u) != 0)
#define LL_DAC_IsActiveFlag_DMAUDR1(reg) (((reg)->SR & 0x2000u) != 0)
#define LL_DAC_ClearFlag_DMAUDR1(reg) ((reg)->SR &= ~0x2000u)
#endif
