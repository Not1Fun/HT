/** @brief Interrupt-buffered display/host UART transport for screen bring-up. */
#ifndef HT_SCREEN_UART_H
#define HT_SCREEN_UART_H

#include <stddef.h>
#include <stdint.h>

enum screen_uart_port {
	SCREEN_UART_DISPLAY = 0,
	SCREEN_UART_HOST
};

struct screen_uart_stats {
	uint32_t rx_bytes;
	uint32_t tx_bytes;
	uint32_t overflows;
	uint32_t uart_errors; /* OR of Zephyr UART error flags. */
};

/* One thread owns read/write/init/stop; the receive IRQ is synchronized internally. */
int screen_uart_init(enum screen_uart_port port);
/* Read returns byte count, zero if empty, or a negative errno. */
int screen_uart_read(enum screen_uart_port port, uint8_t *data, size_t capacity);
/* Write returns zero after local transmission, not a screen acknowledgement. */
int screen_uart_write(enum screen_uart_port port, const uint8_t *data, size_t length);
/* UART errors/overflow latch failure until init; stats remain available after stop. */
int screen_uart_check(enum screen_uart_port port);
int screen_uart_get_stats(enum screen_uart_port port, struct screen_uart_stats *stats);
void screen_uart_stop(enum screen_uart_port port);

#endif
