/** @brief Initialize HT safe pins and check the clock and internal reference. */
#include "platform/board_io.h"
#include "config/analog.h"

#include <errno.h>
#include <soc.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_pwr.h>
#include <stm32_ll_rcc.h>
#include <stm32_ll_system.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define BOARD_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec inputs[] = {
	[BOARD_OC] = GPIO_DT_SPEC_GET(BOARD_NODE, oc_gpios),
	[BOARD_OV] = GPIO_DT_SPEC_GET(BOARD_NODE, ov_gpios),
	[BOARD_SENSOR] = GPIO_DT_SPEC_GET(BOARD_NODE, sensor_gpios),
	[BOARD_COIL] = GPIO_DT_SPEC_GET(BOARD_NODE, coil_gpios),
	[BOARD_AC] = GPIO_DT_SPEC_GET(BOARD_NODE, ac_gpios),
	[BOARD_DC] = GPIO_DT_SPEC_GET(BOARD_NODE, dc_gpios),
	[BOARD_BAT] = GPIO_DT_SPEC_GET(BOARD_NODE, bat_gpios),
	[BOARD_CHARGE] = GPIO_DT_SPEC_GET(BOARD_NODE, charge_gpios),
};
static const struct gpio_dt_spec latch = GPIO_DT_SPEC_GET(BOARD_NODE, latch_gpios);
static const struct gpio_dt_spec buzzer = GPIO_DT_SPEC_GET(BOARD_NODE, buzzer_gpios);
static const struct gpio_dt_spec wdi = GPIO_DT_SPEC_GET(BOARD_NODE, wdi_gpios);
static bool ready;

BUILD_ASSERT(ARRAY_SIZE(inputs) == BOARD_INPUT_COUNT);
BUILD_ASSERT(HT_VREF_MV == 2900U, "VREFBUF SCALE2 requires the 2.9 V configuration");

static int setup_pin(const struct gpio_dt_spec *pin, gpio_flags_t flags)
{
	if (!gpio_is_ready_dt(pin)) {
		return -ENODEV;
	}
	return gpio_pin_configure_dt(pin, flags);
}

static int check_clock(void)
{
#if DT_SAME_NODE(DT_CLOCKS_CTLR(DT_NODELABEL(pll)), DT_NODELABEL(clk_hse))
	if (LL_RCC_PLL_GetMainSource() != LL_RCC_PLLSOURCE_HSE ||
	    !LL_RCC_HSE_IsReady() || (RCC->CR & RCC_CR_HSEBYP) != 0U) {
		return -EIO;
	}
#else
	if (LL_RCC_PLL_GetMainSource() != LL_RCC_PLLSOURCE_HSI ||
	    !LL_RCC_HSI_IsReady()) {
		return -EIO;
	}
#endif
	if (SystemCoreClock != 170000000U ||
	    LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL ||
	    !LL_PWR_IsEnabledRange1BoostMode() ||
	    (PWR->CR3 & PWR_CR3_UCPD_DBDIS) == 0U) {
		return -EIO;
	}
	return 0;
}

static int start_vref(void)
{
	LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);
	LL_VREFBUF_Disable();
	LL_VREFBUF_SetVoltageScaling(LL_VREFBUF_VOLTAGE_SCALE2);
	LL_VREFBUF_DisableHIZ();
	LL_VREFBUF_Enable();

	/* Bounded startup check; this flag does not measure the reference voltage. */
	int64_t end = k_uptime_get() + 20;

	while (!LL_VREFBUF_IsVREFReady()) {
		if (k_uptime_get() >= end) {
			LL_VREFBUF_Disable();
			LL_VREFBUF_EnableHIZ();
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}
	return 0;
}

static int init_board(bool enable_vref)
{
	ready = false;
	if (!enable_vref) {
		LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);
		LL_VREFBUF_Disable();
		LL_VREFBUF_EnableHIZ();
	}
	int rc = setup_pin(&buzzer, GPIO_OUTPUT_INACTIVE);

	if (rc != 0) {
		return rc;
	}
	/* 释放开漏管脚只恢复锁存模式，不会清除已经锁存的报警。 */
	rc = setup_pin(&latch, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return rc;
	}
	/* External watchdog feeding belongs to the future health supervisor. */
	rc = setup_pin(&wdi, GPIO_INPUT);
	if (rc != 0) {
		return rc;
	}
	for (size_t i = 0; i < ARRAY_SIZE(inputs); ++i) {
		rc = setup_pin(&inputs[i], GPIO_INPUT);
		if (rc != 0) {
			return rc;
		}
	}
	rc = check_clock();
	if (rc != 0) {
		return rc;
	}
	rc = enable_vref ? start_vref() : 0;
	ready = rc == 0;
	return rc;
}

int board_io_init(void)
{
	return init_board(true);
}

int board_io_clear_protection(void)
{
	if (!ready) return -EACCES;
	int rc = gpio_pin_set_dt(&latch, 1);
	if (rc == 0) k_busy_wait(10);
	int release_rc = gpio_pin_set_dt(&latch, 0);
	if (rc != 0) return rc;
	if (release_rc != 0) return release_rc;
	k_busy_wait(10);
	return 0;
}

#if defined(CONFIG_HT_SCREEN_TEMPERATURE) || defined(CONFIG_HT_OUTPUT)
int board_io_start_reference(void)
{
	return ready ? start_vref() : -EACCES;
}
#endif

#if defined(CONFIG_HT_SCREEN_BRINGUP)
int board_io_init_digital(void)
{
	return init_board(false);
}
#endif

int board_io_read(uint32_t *state)
{
	if (state == NULL) {
		return -EINVAL;
	}
	if (!ready) {
		return -EACCES;
	}
	uint32_t next = 0;

	for (size_t i = 0; i < ARRAY_SIZE(inputs); ++i) {
		int value = gpio_pin_get_dt(&inputs[i]);

		if (value < 0) {
			ready = false;
			return value;
		}
		if (value != 0) {
			next |= BIT(i);
		}
	}
	*state = next;
	return 0;
}
