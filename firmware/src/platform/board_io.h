/** @brief HT board safe GPIO setup and diagnostic input snapshot. */
#ifndef HT_BOARD_IO_H
#define HT_BOARD_IO_H

#include <stdint.h>

enum board_input {
	BOARD_OC = 0,
	BOARD_OV,
	BOARD_SENSOR,
	BOARD_COIL,
	BOARD_AC,
	BOARD_DC,
	BOARD_BAT,
	BOARD_CHARGE,
	BOARD_INPUT_COUNT
};

int board_io_init(void);
#if defined(CONFIG_HT_SCREEN_BRINGUP)
/* Screen bring-up only: leave the analog reference disabled and high impedance. */
int board_io_init_digital(void);
#endif
#if defined(CONFIG_HT_SCREEN_TEMPERATURE) || defined(CONFIG_HT_OUTPUT)
/* Called by the selected ADC owner after AVDD wiring has been confirmed. */
int board_io_start_reference(void);
#endif
/* Each set bit means the corresponding active-low signal is asserted. */
int board_io_read(uint32_t *state);
/* 仅在输出初始化、DAC静音且继电器已释放后调用；运行期禁止自动清除。 */
int board_io_clear_protection(void);

#endif
