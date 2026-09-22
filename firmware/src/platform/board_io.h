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
/* Each set bit means the corresponding active-low signal is asserted. */
int board_io_read(uint32_t *state);

#endif
