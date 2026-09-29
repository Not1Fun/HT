/* @brief 只处理屏幕与手动请求，始终不启动测量或切换继电器。 */
#include "app/screen_panel.h"
#include "platform/board_io.h"
#include "platform/relay_io.h"
#include "platform/screen_uart.h"
#include "ui/dgus.h"
#include "ui/encoder.h"
#include "ui/panel.h"
#include "ui/view.h"

#include <errno.h>
#include <limits.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(screen_panel);

#define BOARD_NODE DT_PATH(zephyr_user)
#define KEY_COUNT 6u
#define INPUT_MS 10
#define DEBOUNCE_MS 20
#define RELAY_CHECK_MS 200
#define REFRESH_MS 200
#define PROBE_MS 1000
#define PROBE_TIMEOUT_MS 300
#define REBUILD_MS 5000
#define RX_GAP_MS 50
#define VERSION_VP 0x000f

BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(BOARD_NODE, encoder_a_gpios),
			 DT_GPIO_CTLR(BOARD_NODE, encoder_b_gpios)),
	     "Encoder A/B must be sampled from the same GPIO port");
static const struct gpio_dt_spec enc_a = GPIO_DT_SPEC_GET(BOARD_NODE, encoder_a_gpios);
static const struct gpio_dt_spec enc_b = GPIO_DT_SPEC_GET(BOARD_NODE, encoder_b_gpios);

/* 回调对象保持静态寿命；退出先禁用，再注销中断。 */
static struct {
	struct gpio_callback callback;
	struct encoder decoder;
	atomic_t active;
	atomic_t detents;
	atomic_t error;
	bool registered;
} rotary;

struct keys {
	uint8_t candidate;
	uint8_t stable;
	uint8_t armed;
	int64_t changed[KEY_COUNT];
	bool synced;
};

struct link {
	struct dgus_parser parser;
	enum dgus_crc crc;
	bool pending;
	bool response;
	bool connected;
	bool rebuild;
	uint16_t version;
	uint16_t response_version;
	int64_t deadline;
	int64_t next_probe;
	int64_t next_rebuild;
	int64_t last_rx;
};

struct service {
	struct panel panel;
	struct keys keys;
	struct link link;
	struct view view;
	uint32_t digital;
	int64_t next_input;
	int64_t next_relay;
	int64_t next_display;
	int send_error;
	bool link_expired;
};

static int read_encoder(uint8_t *ab)
{
	gpio_port_value_t value;
	int rc = gpio_port_get_raw(enc_a.port, &value);

	if (rc == 0) {
		*ab = ((value & BIT(enc_a.pin)) ? 2u : 0u) |
		      ((value & BIT(enc_b.pin)) ? 1u : 0u);
	}
	return rc;
}

static void encoder_edge(const struct device *port, struct gpio_callback *callback,
			 gpio_port_pins_t pins)
{
	uint8_t ab;
	int8_t step;
	int rc;

	ARG_UNUSED(port);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);
	if (!atomic_get(&rotary.active) || atomic_get(&rotary.error)) {
		return;
	}
	rc = read_encoder(&ab);
	if (rc == 0) {
		rc = encoder_update(&rotary.decoder, ab, &step);
	}
	if (rc != 0) {
		atomic_cas(&rotary.error, 0, rc);
		return;
	}
	if (step != 0) {
		atomic_val_t count = atomic_get(&rotary.detents);

		if ((step > 0 && count == INT32_MAX) || (step < 0 && count == INT32_MIN)) {
			atomic_cas(&rotary.error, 0, -EOVERFLOW);
			return;
		}
		atomic_add(&rotary.detents, step);
	}
}

static int stop_encoder(void)
{
	int rc = 0;
	int next;

	atomic_clear(&rotary.active);
	if (rotary.registered) {
		rc = gpio_pin_interrupt_configure_dt(&enc_a, GPIO_INT_DISABLE);
		next = gpio_pin_interrupt_configure_dt(&enc_b, GPIO_INT_DISABLE);
		if (rc == 0) {
			rc = next;
		}
		next = gpio_remove_callback(enc_a.port, &rotary.callback);
		if (rc == 0) {
			rc = next;
		}
		rotary.registered = false;
	}
	return rc;
}

static int start_encoder(void)
{
	const struct encoder_config config = {.transitions_per_detent = 4, .reversed = false};
	uint8_t ab;
	int rc;

	if (!gpio_is_ready_dt(&enc_a) || !gpio_is_ready_dt(&enc_b)) {
		return -ENODEV;
	}
	atomic_clear(&rotary.active);
	atomic_clear(&rotary.detents);
	atomic_clear(&rotary.error);
	rc = gpio_pin_configure_dt(&enc_a, GPIO_INPUT);
	if (rc == 0) {
		rc = gpio_pin_configure_dt(&enc_b, GPIO_INPUT);
	}
	if (rc != 0) {
		return rc;
	}
	gpio_init_callback(&rotary.callback, encoder_edge, BIT(enc_a.pin) | BIT(enc_b.pin));
	rc = gpio_add_callback(enc_a.port, &rotary.callback);
	if (rc != 0) {
		return rc;
	}
	rotary.registered = true;
	rc = gpio_pin_interrupt_configure_dt(&enc_a, GPIO_INT_EDGE_BOTH);
	if (rc == 0) {
		rc = gpio_pin_interrupt_configure_dt(&enc_b, GPIO_INT_EDGE_BOTH);
	}
	if (rc != 0) {
		return rc;
	}
	unsigned int key = irq_lock();

	rc = read_encoder(&ab);
	if (rc == 0) {
		rc = encoder_init(&rotary.decoder, &config, ab);
	}
	if (rc == 0) {
		atomic_set(&rotary.active, 1);
	}
	irq_unlock(key);
	return rc;
}

static int update_keys(struct service *service, uint8_t raw, int64_t now)
{
	struct keys *keys = &service->keys;
	uint8_t pressed = panel_pressed_keys(raw);

	if (!keys->synced) {
		keys->candidate = pressed;
		keys->stable = pressed;
		keys->armed = (uint8_t)(~pressed & 0x3fu);
		keys->synced = true;
		return 0;
	}
	for (unsigned int i = 0; i < KEY_COUNT; ++i) {
		uint8_t bit = (uint8_t)BIT(i);

		if ((pressed ^ keys->candidate) & bit) {
			keys->candidate ^= bit;
			keys->changed[i] = now;
		}
		if (!((keys->candidate ^ keys->stable) & bit) ||
		    now - keys->changed[i] < DEBOUNCE_MS) {
			continue;
		}
		keys->stable ^= bit;
		if (!(keys->stable & bit)) {
			keys->armed |= bit;
		} else if (keys->armed & bit) {
			keys->armed &= (uint8_t)~bit;
			int action = panel_key(&service->panel, (enum panel_key)i);

			if (action < 0) {
				return action;
			}
			if (action != PANEL_ACTION_NONE) {
				LOG_INF("Request only: range=%u ohm frequency=%u Hz; output disabled",
					panel_range_ohm(service->panel.range_index),
					service->panel.frequency_hz);
			}
		}
	}
	return 0;
}

static int update_inputs(struct service *service)
{
	int64_t now = k_uptime_get();
	uint8_t raw;
	int rc = (int)atomic_get(&rotary.error);

	if (rc != 0 || now < service->next_input) {
		return rc;
	}
	service->next_input = now + INPUT_MS;
	if (now >= service->next_relay) {
		rc = relay_io_check();
		if (rc != 0) {
			return rc;
		}
		service->next_relay = now + RELAY_CHECK_MS;
	}
	rc = relay_io_read_panel(&raw);
	if (rc == 0) {
		rc = board_io_read(&service->digital);
	}
	if (rc != 0) {
		return rc;
	}
	/* 目标始终为0，独立使能只更新安全模型，不存在START执行路径。 */
	(void)panel_inputs(&service->panel, PANEL_CC, !(raw & BIT(6)),
		(service->digital & (BIT(BOARD_OC) | BIT(BOARD_OV))) != 0);
	rc = update_keys(service, raw, k_uptime_get());
	if (rc == 0) {
		int32_t detents = (int32_t)atomic_set(&rotary.detents, 0);

		rc = panel_rotate(&service->panel, detents);
	}
	return rc;
}

static void receive_frame(void *ctx, const struct dgus_frame *frame)
{
	struct link *link = ctx;

	if (link->pending && k_uptime_get() <= link->deadline &&
	    frame->kind == DGUS_READ_DATA && frame->vp == VERSION_VP && frame->count == 1 &&
	    frame->words[0] != 0 && frame->words[0] != 0xffff) {
		link->response_version = frame->words[0];
		link->response = true;
		link->pending = false;
	}
}

static int update_link(struct service *service)
{
	struct link *link = &service->link;
	uint8_t bytes[64];
	int64_t now = k_uptime_get();
	int rc = screen_uart_check(SCREEN_UART_DISPLAY);

	if (rc != 0) {
		return rc;
	}
	/* 每次最多取256字节，噪声持续输入也不阻止面板与全断检查。 */
	for (unsigned int batch = 0; batch < 4; ++batch) {
		rc = screen_uart_read(SCREEN_UART_DISPLAY, bytes, sizeof(bytes));
		if (rc < 0) {
			return rc;
		}
		if (rc == 0) {
			break;
		}
		if (now - link->last_rx >= RX_GAP_MS) {
			dgus_reset(&link->parser);
		}
		link->last_rx = now;
		rc = dgus_feed(&link->parser, bytes, (size_t)rc);
		if (rc != DGUS_OK) {
			return rc;
		}
	}
	if (link->response) {
		link->response = false;
		if (!link->connected || link->version != link->response_version) {
			LOG_INF("Display response: version=0x%04x CRC=%s",
				link->response_version, link->crc == DGUS_CRC_NONE ? "none" : "modbus");
		}
		/* 同版本快速重启可能短于探测间隔；低频重设库/页使其自行恢复。 */
		if (!link->connected || link->version != link->response_version ||
		    now >= link->next_rebuild) {
			link->rebuild = true;
			link->next_rebuild = now + REBUILD_MS;
			view_reset(&service->view);
		}
		link->version = link->response_version;
		link->connected = true;
		link->next_probe = now + PROBE_MS;
	}
	if (link->pending && now >= link->deadline) {
		if (link->connected) {
			LOG_WRN("Display response timeout; VP writes stopped");
		}
		link->connected = false;
		link->pending = false;
		link->crc = link->crc == DGUS_CRC_NONE ? DGUS_CRC_MODBUS : DGUS_CRC_NONE;
		link->next_probe = now;
		view_reset(&service->view);
		rc = dgus_init(&link->parser, link->crc, receive_frame, link);
		if (rc != DGUS_OK) {
			return rc;
		}
	}
	if (!link->pending && now >= link->next_probe) {
		int length = dgus_encode_read(link->crc, VERSION_VP, 1, bytes, sizeof(bytes));

		if (length < 0) {
			return length;
		}
		dgus_reset(&link->parser);
		link->pending = true;
		link->deadline = now + PROBE_TIMEOUT_MS;
		rc = screen_uart_write(SCREEN_UART_DISPLAY, bytes, (size_t)length);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int display_send(void *ctx, const uint8_t *data, size_t length)
{
	struct service *service = ctx;
	int rc = update_inputs(service);

	/* 各VP短帧之间继续10ms输入调度；旋转边沿始终由ISR捕获。 */
	if (rc == 0 && (!service->link.connected ||
	    (service->link.pending && k_uptime_get() >= service->link.deadline))) {
		service->link_expired = true;
		rc = -EAGAIN;
	}
	if (rc == 0) {
		rc = screen_uart_write(SCREEN_UART_DISPLAY, data, length);
	}
	service->send_error = rc;
	return rc;
}

static void snapshot(const struct service *service, struct view_snapshot *out)
{
	const struct panel *panel = &service->panel;
	bool fault = (service->digital & (BIT(BOARD_OC) | BIT(BOARD_OV))) != 0;

	*out = (struct view_snapshot){
		.page = panel->page, .state = fault ? VIEW_FAULT : VIEW_STANDBY,
		.battery = VIEW_POWER_UNKNOWN, .selected = panel->field,
		.editing = panel_draft_changed(panel), .fresh = true,
	};
	out->values[VIEW_RANGE_CHOICE] = (struct view_value){
		panel_range_ohm(panel->field == PANEL_RANGE ? panel->draft_index : panel->range_index),
		true};
	out->values[VIEW_FREQUENCY_CHOICE] = (struct view_value){
		panel->field == PANEL_FREQUENCY ? panel_frequency_hz(panel->draft_index) :
		panel->frequency_hz, true};
	/* 其余字段无测量/应用状态拥有者，保持invalid，不能把请求当作实值。 */
}

static int refresh_display(struct service *service)
{
	struct view_snapshot value;
	int rc;

	service->send_error = 0;
	service->link_expired = false;
	if (service->link.rebuild) {
		uint8_t frame[16];
		const uint16_t library[] = {0x5a00, 0x0020};
		int length = dgus_encode_write(service->link.crc, 0x00de, library,
			ARRAY_SIZE(library), frame, sizeof(frame));

		if (length < 0) {
			return length;
		}
		rc = display_send(service, frame, (size_t)length);
		if (rc != 0) {
			return rc;
		}
		service->link.rebuild = false;
	}
	snapshot(service, &value);
	rc = view_refresh(&service->view, &value, service->link.crc, display_send, service);
	return rc == VIEW_OK ? 0 : (service->send_error != 0 ? service->send_error : -EIO);
}

int screen_panel_run(void)
{
	struct service service = {.link.crc = DGUS_CRC_NONE};
	const struct panel_config limits = {.current_max_ma = 7070, .apparent_max_mva = 50000};
	int rc = panel_init(&service.panel, &limits, PANEL_CC, true, false);

	if (rc == 0) {
		rc = dgus_init(&service.link.parser, service.link.crc, receive_frame, &service.link);
	}
	if (rc == 0) {
		rc = screen_uart_init(SCREEN_UART_DISPLAY);
	}
	if (rc == 0) {
		rc = start_encoder();
	}
	LOG_INF("Screen panel diagnostics: targets zero, requests only, output disabled");
	while (rc == 0) {
		rc = update_inputs(&service);
		if (rc == 0) {
			rc = update_link(&service);
		}
		if (rc == 0 && service.link.connected && k_uptime_get() >= service.next_display) {
			service.next_display = k_uptime_get() + REFRESH_MS;
			rc = refresh_display(&service);
			if (rc != 0 && service.link_expired) {
				/* 回读期限在短帧间到达，下一轮只探测，不继续VP写入。 */
				rc = 0;
			}
		}
		if (rc == 0) {
			k_msleep(1);
		}
	}
	int stop_rc = stop_encoder();

	screen_uart_stop(SCREEN_UART_DISPLAY);
	if (stop_rc != 0) {
		LOG_ERR("Encoder interrupt cleanup failed: %d", stop_rc);
	}
	return rc != 0 ? rc : stop_rc;
}
