/** @brief Single-owner relay-off checks and panel reads for safe bring-up. */
#ifndef HT_RELAY_IO_H
#define HT_RELAY_IO_H

#include <stdint.h>

int relay_io_init(void);
int relay_io_check(void);
int relay_io_read_panel(uint8_t *state);
/* One best-effort all-off attempt; remains unavailable even on success. */
int relay_io_stop(void);

#endif
