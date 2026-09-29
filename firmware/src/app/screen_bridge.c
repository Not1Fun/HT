/** @brief Forward USART1/USART3 bytes while periodically verifying relay-off registers. */
#include "app/screen_bridge.h"
#include "platform/relay_io.h"
#include "platform/screen_uart.h"

#include <zephyr/kernel.h>

static int forward(enum screen_uart_port source, enum screen_uart_port destination)
{
	uint8_t bytes[64];
	int count = screen_uart_read(source, bytes, sizeof(bytes));

	if (count <= 0) {
		return count;
	}
	int rc = screen_uart_write(destination, bytes, (size_t)count);

	return rc != 0 ? rc : count;
}

int screen_bridge_run(void)
{
	int rc = screen_uart_init(SCREEN_UART_DISPLAY);

	if (rc == 0) {
		rc = screen_uart_init(SCREEN_UART_HOST);
	}
	int64_t next_check = k_uptime_get();

	while (rc == 0) {
		if (k_uptime_get() >= next_check) {
			rc = relay_io_check();
			if (rc != 0) {
				break;
			}
			next_check = k_uptime_get() + 200;
		}
		int to_screen = forward(SCREEN_UART_HOST, SCREEN_UART_DISPLAY);

		if (to_screen < 0) {
			rc = to_screen;
			break;
		}
		int to_host = forward(SCREEN_UART_DISPLAY, SCREEN_UART_HOST);

		if (to_host < 0) {
			rc = to_host;
			break;
		}
		if (to_screen == 0 && to_host == 0) {
			k_msleep(1);
		}
	}
	screen_uart_stop(SCREEN_UART_HOST);
	screen_uart_stop(SCREEN_UART_DISPLAY);
	return rc;
}
