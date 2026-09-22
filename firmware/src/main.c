/** @brief Run the output-disabled bring-up checks and change-only diagnostics. */
#include "platform/board_io.h"
#include "platform/relay_io.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ht);

int main(void)
{
	LOG_INF("HT bring-up: output disabled, console 115200 8N1");
	int rc = board_io_init();
	/* Attempt all-off even if the board/reference checks fail. */
	int relay_rc = relay_io_init();

	if (rc != 0 || relay_rc != 0) {
		LOG_ERR("Startup failed: board=%d relay=%d; reset required", rc, relay_rc);
		return 0;
	}
	LOG_INF("Clock configuration checked: 170 MHz; VREFBUF ready flag set");
	LOG_INF("Relay latch/config verified off; physical contacts unverified");
	LOG_INF("Input bits: OC OV SENSOR COIL AC DC BAT CHARGE (bit 0..7)");

	uint32_t prev = UINT32_MAX;
	uint8_t prev_panel = 0;
	bool first = true;

	for (;;) {
		uint32_t state;
		uint8_t panel;

		rc = relay_io_check();
		if (rc == 0) {
			rc = relay_io_read_panel(&panel);
		}
		if (rc == 0) {
			rc = board_io_read(&state);
		}
		if (rc != 0) {
			int off_rc = relay_io_stop();

			LOG_ERR("Check failed: %d, all-off attempt: %d; reset required", rc, off_rc);
			return 0;
		}
		if (first || state != prev || panel != prev_panel) {
			LOG_INF("Inputs=0x%02x panel_raw=0x%02x; output disabled", state, panel);
			prev = state;
			prev_panel = panel;
			first = false;
		}
		k_msleep(200);
	}
}
