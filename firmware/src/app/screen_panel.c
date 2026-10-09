/* @brief 面板导航、温度和日志；正式输出自动匹配，频率和VA由面板提交。 */
#include "app/screen_panel.h"
#include "platform/board_io.h"
#include "platform/relay_io.h"
#include "platform/screen_uart.h"
#if defined(CONFIG_HT_SCREEN_TEMPERATURE)
#include "platform/temperature_io.h"
#endif
#if defined(CONFIG_HT_DDS_BENCH)
#include "app/dds_bench.h"
#endif
#if defined(CONFIG_HT_OUTPUT)
#include "app/output.h"
#endif
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
#define TEMPERATURE_MS 200
#define TEMPERATURE_STALE_MS 1000
#define BENCH_MVPP 100u
#define BENCH_SECONDS 60u

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
	struct event_log log;
	uint8_t log_offset;
	bool digital_valid;
	uint32_t digital;
	uint8_t raw_panel;
#if defined(CONFIG_HT_OUTPUT)
	uint8_t matched_range;
	bool match_seen;
#endif
#if defined(CONFIG_HT_SCREEN_TEMPERATURE)
	struct temperature_snapshot temperature;
	bool temperature_active;
	bool temperature_seen;
	int64_t next_temperature;
	int64_t last_temperature;
#endif
	int64_t next_input;
	int64_t next_relay;
	int64_t next_display;
	int send_error;
	bool link_expired;
#if defined(CONFIG_HT_DDS_BENCH) || defined(CONFIG_HT_OUTPUT)
	bool dds_ready;
	bool dds_running;
	bool dds_failed;
#endif
};

static void record_event(struct service *service, enum event_kind kind, int32_t value);

static void request_stop(void)
{
#if defined(CONFIG_HT_OUTPUT)
	output_stop();
#elif defined(CONFIG_HT_DDS_BENCH)
	(void)dds_bench_request_stop();
#endif
}

static int request_start(struct service *service)
{
#if defined(CONFIG_HT_OUTPUT)
	return output_start(service->panel.range, service->panel.frequency_hz, service->panel.apparent_mva);
#elif defined(CONFIG_HT_DDS_BENCH)
	return dds_bench_request_start(service->panel.frequency_hz, BENCH_MVPP, BENCH_SECONDS);
#else
	ARG_UNUSED(service);
	return -ENOTSUP;
#endif
}

#if defined(CONFIG_HT_OUTPUT)
static void update_output(struct service *service, bool io_ok)
{
	output_inputs(service->raw_panel, service->link.connected &&
		!(service->link.pending && k_uptime_get() >= service->link.deadline), io_ok && service->digital_valid);
	struct output_snapshot value;
	output_snapshot(&value);
	if (value.fault && !service->dds_failed) {
		record_event(service, EVENT_DDS_FAILED, value.error);
		service->dds_failed = true;
	}
	if (value.active != service->dds_running) {
		record_event(service, value.active ? EVENT_DDS_START : EVENT_DDS_STOP,
			value.active ? (int32_t)service->panel.frequency_hz : 0);
		service->dds_running = value.active;
	}
	if (!value.active) service->match_seen = false;
	if (value.running && value.range < PANEL_RANGE_COUNT &&
	    (!service->match_seen || service->matched_range != value.range)) {
		record_event(service, EVENT_RANGE, (int32_t)panel_range_ohm(value.range));
		service->matched_range = value.range;
		service->match_seen = true;
	}
#if defined(CONFIG_HT_OUTPUT_BENCH)
    if (service->panel.debug.wave != value.debug.trial && service->panel.debug.field == 3)
        service->panel.debug.draft = value.debug.trial ? 1 : 0;
    service->panel.debug.busy = value.debug.busy;
    service->panel.debug.wave = value.debug.trial;
#endif
	(void)panel_set_output_state(&service->panel, value.running || value.switching,
		value.available && service->panel.apparent_mva > 0);
}
#endif

#if defined(CONFIG_HT_DDS_BENCH)
static void update_dds(struct service *service, bool io_ok)
{
	int64_t now = k_uptime_get();
	bool permitted = io_ok && service->digital_valid && service->link.connected &&
		!(service->digital & (BIT(BOARD_OC) | BIT(BOARD_OV))) &&
		!(service->link.pending && now >= service->link.deadline) &&
		service->temperature_active && service->temperature_seen &&
		now - service->last_temperature < TEMPERATURE_STALE_MS;

	if (service->dds_ready) {
		int rc = dds_bench_poll(permitted);

		if (rc != 0) {
			service->dds_ready = false;
			(void)dds_bench_stop();
			LOG_ERR("DDS bench stopped: %d; reset required", rc);
		}
	}
	struct dds_bench_snapshot dds;

	if (dds_bench_snapshot(&dds) == 0) {
		if (dds.faulted && !service->dds_failed) {
			record_event(service, EVENT_DDS_FAILED, dds.last_error);
			service->dds_failed = true;
		}
		if (dds.running != service->dds_running) {
			record_event(service, dds.running ? EVENT_DDS_START : EVENT_DDS_STOP,
				dds.running ? (int32_t)dds.frequency_hz : 0);
			service->dds_running = dds.running;
		}
		(void)panel_set_output_state(&service->panel, dds.running,
			service->dds_ready && dds.ready && dds.permitted && !dds.faulted);
	}
}
#endif

static void scroll_log(struct service *service, int32_t steps)
{
	size_t count = event_log_count(&service->log);
	int64_t limit = count > VIEW_LOG_ROWS ? count - VIEW_LOG_ROWS : 0;
	int64_t next = (int64_t)service->log_offset + steps;

	service->log_offset = (uint8_t)CLAMP(next, 0, limit);
}

static void record_event(struct service *service, enum event_kind kind, int32_t value)
{
	(void)event_log_add(&service->log, (uint32_t)(k_uptime_get() / 1000), kind, value);
	/* 翻看历史时尽量保持当前位置；最新视图随新事件更新。 */
	scroll_log(service, service->log_offset != 0 ? 1 : 0);
}

static void update_digital(struct service *service, uint32_t state)
{
	const enum board_input pins[] = {BOARD_BAT, BOARD_OC, BOARD_OV};
	const enum event_kind active[] = {EVENT_BATTERY_OK, EVENT_OC_ACTIVE, EVENT_OV_ACTIVE};
	const enum event_kind inactive[] = {EVENT_BATTERY_ALARM, EVENT_OC_CLEAR, EVENT_OV_CLEAR};

	for (size_t i = 0; i < ARRAY_SIZE(pins); ++i) {
		bool on = (state & BIT(pins[i])) != 0;
		if ((!service->digital_valid && (i == 0 || on)) ||
		    (service->digital_valid && ((service->digital ^ state) & BIT(pins[i])))) {
			record_event(service, on ? active[i] : inactive[i], 0);
		}
	}
	service->digital = state;
	service->digital_valid = true;
}

#if defined(CONFIG_HT_SCREEN_TEMPERATURE)
static void update_temperature(struct service *service)
{
	int64_t now = k_uptime_get();
	struct temperature_snapshot next;

	if (!service->temperature_active || now < service->next_temperature) {
		return;
	}
	service->next_temperature = now + TEMPERATURE_MS;
	int rc = temperature_io_read(&next);

	if (rc != 0) {
		temperature_io_stop();
		service->temperature_active = false;
		record_event(service, EVENT_TEMP_INIT_FAILED, rc);
		LOG_WRN("Temperature sampling stopped: %d", rc);
	}
	for (size_t i = 0; i < TEMPERATURE_CHANNEL_COUNT; ++i) {
		bool valid = rc == 0 && next.samples[i].status == NTC_OK;
		bool previous = service->temperature.samples[i].status == NTC_OK;

		if (!service->temperature_seen || previous != valid) {
			record_event(service, valid ? EVENT_TEMP_READY : EVENT_TEMP_INVALID, (int32_t)i + 1);
		}
	}
	service->temperature = next;
	service->temperature_seen = true;
	service->last_temperature = now;
}
#endif

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
	uint8_t events = 0;
	bool stopping = false;

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
			events |= bit;
		}
	}
	/* 同轮状态页的停止键先于导航与确认；请求队列拒绝越过STOP的新启动。 */
	if (service->panel.page == PANEL_PAGE_STATUS && (events & BIT(PANEL_KEY_DOWN))) {
		request_stop();
		stopping = true;
		events &= (uint8_t)~BIT(PANEL_KEY_DOWN);
	}
	for (unsigned int i = 0; i < KEY_COUNT; ++i) {
		if (events & BIT(i)) {
			enum panel_page before = service->panel.page;
			if (before == PANEL_PAGE_LOG) {
				if (i == PANEL_KEY_UP || i == PANEL_KEY_DOWN) {
					scroll_log(service, i == PANEL_KEY_UP ? -1 : 1);
				} else if (i == PANEL_KEY_OK || i == PANEL_KEY_ENCODER) {
					service->log_offset = 0;
				}
			}
			int action = panel_key(&service->panel, (enum panel_key)i);

			if (action < 0) {
				return action;
			}
#if defined(CONFIG_HT_OUTPUT_BENCH)
            if (action == PANEL_ACTION_DEBUG_PARAMS) { request_stop(); stopping = true; }
            if (!stopping && (action == PANEL_ACTION_DEBUG_RELAY || action == PANEL_ACTION_DEBUG_WAVE)) {
                struct panel *p = &service->panel;
                uint8_t relay = p->page == PANEL_PAGE_DAC ? 0 : p->debug.choice[0];
                int rc = output_debug(relay, panel_frequency_hz(p->debug.choice[1]),
                    panel_debug_mvpp(p->debug.choice[2]), action == PANEL_ACTION_DEBUG_WAVE);
                if (rc != 0) { p->debug.draft = 0; LOG_WRN("Debug request rejected: %d", rc); }
            }
#endif
			if (action == PANEL_ACTION_FREQUENCY) {
				request_stop();
				stopping = true;
				record_event(service, EVENT_FREQUENCY, (int32_t)service->panel.frequency_hz);
				LOG_INF("Saved frequency request: %u Hz", service->panel.frequency_hz);
			}
			if (action == PANEL_ACTION_OUTPUT_STOP || action == PANEL_ACTION_POWER || action == PANEL_ACTION_RANGE) {
				request_stop();
				stopping = true;
			} else if (action == PANEL_ACTION_OUTPUT_START && !stopping) {
				int rc = request_start(service);

				if (rc != 0) {
					service->panel.draft_index = 0;
					LOG_WRN("Output start request rejected: %d", rc);
				}
			}
			if (before != service->panel.page && service->panel.page == PANEL_PAGE_LOG) {
				service->log_offset = 0;
			}
		}
	}
	return 0;
}

static int update_inputs(struct service *service)
{
	int64_t now = k_uptime_get();
	uint8_t raw;
	uint32_t digital;
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
		rc = board_io_read(&digital);
	}
	if (rc != 0) {
		return rc;
	}
	update_digital(service, digital);
	service->raw_panel = raw;
#if defined(CONFIG_HT_DDS_BENCH) || defined(CONFIG_HT_OUTPUT)
	/* 物理使能释放会停止输出，启动仍需显式确认。 */
	if (service->panel.enabled && (raw & BIT(6))) {
		request_stop();
	}
#endif
	/* 目标值由面板保存，输出请求单独提交给输出服务。 */
	(void)panel_inputs(&service->panel, PANEL_VA, !(raw & BIT(6)),
		(service->digital & (BIT(BOARD_OC) | BIT(BOARD_OV))) != 0);
	rc = update_keys(service, raw, k_uptime_get());
	if (rc == 0) {
		int32_t detents = (int32_t)atomic_set(&rotary.detents, 0);

		if (service->panel.page == PANEL_PAGE_LOG) {
			scroll_log(service, detents);
		} else {
			rc = panel_rotate(&service->panel, detents);
		}
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
			record_event(service, EVENT_DISPLAY_ONLINE, link->response_version);
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
			record_event(service, EVENT_DISPLAY_OFFLINE, 0);
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
#if defined(CONFIG_HT_OUTPUT)
	update_output(service, rc == 0);
#endif
#if defined(CONFIG_HT_DDS_BENCH)
	update_dds(service, rc == 0);
#endif
	return rc;
}

#if defined(CONFIG_HT_OUTPUT)
static uint8_t blocking_reason(uint32_t blocked, int error, int64_t now)
{
    uint8_t reasons[OUTPUT_REASON_COUNT], count = 0;
    for (unsigned int reason = 1; reason < OUTPUT_REASON_COUNT; ++reason) {
        if (!(blocked & OUTPUT_REASON_BIT(reason))) continue;
        reasons[count++] = reason == OUTPUT_REASON_FAULT && error >= POWER_ERROR_IO &&
            error <= POWER_ERROR_MATCH ? OUTPUT_REASON_COUNT + (unsigned int)error - 1u : reason;
    }
    return count ? reasons[(uint64_t)now / 2000u % count] : OUTPUT_REASON_NONE;
}
#endif

static void snapshot(const struct service *service, struct view_snapshot *out)
{
	const struct panel *panel = &service->panel;
	bool fault = (service->digital & (BIT(BOARD_OC) | BIT(BOARD_OV))) != 0;

	*out = (struct view_snapshot){
		.page = panel->page, .state = fault ? VIEW_FAULT : VIEW_STANDBY,
		.battery = !service->digital_valid ? VIEW_POWER_UNKNOWN :
			(service->digital & BIT(BOARD_BAT)) ? VIEW_POWER_NORMAL : VIEW_POWER_ALARM,
		.selected = panel->field,
		.output = VIEW_OUTPUT_DISABLED,
		.editing = panel_draft_changed(panel), .fresh = true,
	};
	out->values[VIEW_RANGE_CHOICE] = (struct view_value){
		panel_range_ohm(panel->field == PANEL_RANGE ? panel->draft_index : panel->range), true};
	out->values[VIEW_FREQUENCY_CHOICE] = (struct view_value){
		panel->field == PANEL_FREQUENCY ? panel_frequency_hz(panel->draft_index) :
		panel->frequency_hz, true};
	out->values[VIEW_POWER_CHOICE] = (struct view_value){
		panel->field == PANEL_POWER ? (uint32_t)panel->draft_index * 1000u : panel->apparent_mva, true};
	out->log_count = (uint8_t)event_log_count(&service->log);
	out->log_offset = service->log_offset;
	for (size_t row = 0; row < VIEW_LOG_ROWS; ++row) {
		(void)event_log_get(&service->log, service->log_offset + row, &out->logs[row]);
	}
#if defined(CONFIG_HT_SCREEN_TEMPERATURE)
	for (size_t i = 0; i < TEMPERATURE_CHANNEL_COUNT; ++i) {
		out->values[VIEW_NTC1 + i] = (struct view_value){
			service->temperature.samples[i].decicelsius,
			service->temperature_active && service->temperature_seen &&
			k_uptime_get() - service->last_temperature < TEMPERATURE_STALE_MS &&
			service->temperature.samples[i].status == NTC_OK};
	}
#endif

#if defined(CONFIG_HT_DDS_BENCH)
	struct dds_bench_snapshot dds;

	out->output = service->dds_failed ? VIEW_OUTPUT_FAULT : VIEW_OUTPUT_UNAVAILABLE;
	if (dds_bench_snapshot(&dds) == 0) {
		if (dds.faulted || service->dds_failed) {
			out->state = VIEW_FAULT;
			out->output = VIEW_OUTPUT_FAULT;
		} else if (dds.running && !fault) {
			out->state = VIEW_RUNNING;
			out->output = VIEW_OUTPUT_RUNNING;
			out->values[VIEW_FREQUENCY] = (struct view_value){dds.frequency_hz, true};
			out->values[VIEW_ELAPSED] = (struct view_value){dds.elapsed_seconds, true};
		} else if (panel->output_available) {
			out->output = panel->field == PANEL_OUTPUT && panel->draft_index != 0 ?
				VIEW_OUTPUT_ARMED : VIEW_OUTPUT_OFF;
		}
	}
#endif
	/* 其余字段无测量/应用状态拥有者，保持invalid，不能把请求当作实值。 */
#if defined(CONFIG_HT_OUTPUT)
	struct output_snapshot value;
	output_snapshot(&value);
	out->state = value.fault ? VIEW_FAULT : value.switching ? VIEW_SWITCHING : value.running ? VIEW_RUNNING : VIEW_STANDBY;
	out->output = value.fault ? VIEW_OUTPUT_FAULT : value.running || value.switching ? VIEW_OUTPUT_RUNNING :
		!panel->output_available ? VIEW_OUTPUT_UNAVAILABLE :
		panel->field == PANEL_OUTPUT && panel->draft_index ? VIEW_OUTPUT_ARMED : VIEW_OUTPUT_OFF;
	if (value.active) {
		out->values[VIEW_RANGE] = (struct view_value){panel_range_ohm(value.range), value.range < PANEL_RANGE_COUNT};
		out->values[VIEW_FREQUENCY] = (struct view_value){value.frequency, value.frequency != 0};
	}
	if (value.running) {
		out->values[VIEW_FREQUENCY] = (struct view_value){value.frequency, true};
		out->values[VIEW_ELAPSED] = (struct view_value){value.elapsed_seconds, true};
	}
#if defined(CONFIG_HT_OUTPUT_BENCH)
    out->debug.field = panel->debug.field;
    out->debug.relay = panel->debug.field == 0 ? panel->debug.draft : panel->debug.choice[0];
    if (panel->page == PANEL_PAGE_DAC) out->debug.relay = 0;
    out->debug.frequency = panel_frequency_hz(panel->debug.field == 1 ? panel->debug.draft : panel->debug.choice[1]);
    out->debug.millivolts_pp = panel_debug_mvpp(panel->debug.field == 2 ? panel->debug.draft : panel->debug.choice[2]);
    out->debug.coils = value.debug.coils;
    out->debug.seconds = value.debug.seconds;
    bool available = out->debug.relay == 0 ? value.debug.dac_available : value.available;
    out->debug.state = value.fault ? 4 : !available ? 3 : value.debug.wave ? 2 : value.debug.busy ? 1 : 0;
    out->debug.starting = value.debug.trial && !value.debug.wave;
    out->debug.pending = panel->debug.field == 3 && panel->debug.draft && !value.debug.trial;
#endif
    uint32_t blocked = value.blocked;
#if defined(CONFIG_HT_OUTPUT_BENCH)
    if (panel->page >= PANEL_PAGE_DEBUG && out->debug.relay == 0) blocked = value.debug.blocked;
#endif
    if (!value.ready) blocked |= OUTPUT_REASON_BIT(OUTPUT_REASON_INIT);
    if (panel->page == PANEL_PAGE_SETTINGS && panel->apparent_mva == 0)
        blocked |= OUTPUT_REASON_BIT(OUTPUT_REASON_TARGET);
    out->reason = blocking_reason(blocked, value.error, k_uptime_get());
	int64_t age = k_uptime_get() - value.signal.reading.time_ms;
	bool valid = value.signal.reading.valid && age >= 0 && age < 150;
	out->values[VIEW_CURRENT] = (struct view_value){value.signal.reading.current_ma, valid};
	out->values[VIEW_VOLTAGE] = (struct view_value){value.signal.reading.voltage_mv, valid};
	for (size_t i = 0; i < 3; ++i) out->values[VIEW_NTC1 + i] = (struct view_value){
		value.signal.temperature.samples[i].decicelsius,
		value.ready && k_uptime_get() - value.signal.temperature_ms < 1000 &&
		value.signal.temperature.samples[i].status == NTC_OK};
#endif
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
	int rc = panel_init(&service.panel, &limits, PANEL_VA, true, false);
#if defined(CONFIG_HT_OUTPUT_BENCH)
    service.panel.debug_enabled = true;
#endif
	event_log_init(&service.log);

	if (rc == 0) {
		rc = dgus_init(&service.link.parser, service.link.crc, receive_frame, &service.link);
	}
	if (rc == 0) {
		rc = screen_uart_init(SCREEN_UART_DISPLAY);
	}
	if (rc == 0) {
		rc = start_encoder();
	}
	if (rc == 0) {
#if defined(CONFIG_HT_OUTPUT_BENCH)
		record_event(&service, EVENT_BOOT, EVENT_BOOT_BENCH);
#else
		record_event(&service, EVENT_BOOT, EVENT_BOOT_NORMAL);
#endif
#if defined(CONFIG_HT_OUTPUT)
		int output_rc = output_init();
		if (output_rc != 0) {
			service.dds_failed = true;
			record_event(&service, EVENT_DDS_FAILED, output_rc);
			LOG_ERR("Output initialization failed: %d", output_rc);
		}
#endif
#if defined(CONFIG_HT_SCREEN_TEMPERATURE)
		int temp_rc = temperature_io_init();

		service.temperature_active = temp_rc == 0;
		if (temp_rc != 0) {
			record_event(&service, EVENT_TEMP_INIT_FAILED, temp_rc);
			LOG_WRN("Temperature initialization failed: %d", temp_rc);
		}
#endif
#if defined(CONFIG_HT_DDS_BENCH)
		int dds_rc = service.temperature_active ? dds_bench_init() : -EACCES;

		service.dds_ready = dds_rc == 0;
		if (dds_rc != 0) {
			service.dds_failed = true;
			record_event(&service, EVENT_DDS_FAILED, dds_rc);
			LOG_ERR("DDS bench unavailable: %d", dds_rc);
		}
#endif
	}
	LOG_INF("Screen panel: output starts only after explicit confirmation");
	while (rc == 0) {
		rc = update_inputs(&service);
#if defined(CONFIG_HT_SCREEN_TEMPERATURE)
		if (rc == 0) {
			update_temperature(&service);
		}
#endif
		if (rc == 0) {
			rc = update_link(&service);
		}
#if defined(CONFIG_HT_DDS_BENCH)
		update_dds(&service, rc == 0);
#endif
#if defined(CONFIG_HT_OUTPUT)
		update_output(&service, rc == 0);
#endif
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
#if defined(CONFIG_HT_DDS_BENCH)
	(void)dds_bench_stop();
#endif
#if defined(CONFIG_HT_OUTPUT)
	output_shutdown();
#endif
	int stop_rc = stop_encoder();
	if (rc != 0) {
		record_event(&service, EVENT_IO_ERROR, rc);
	}
#if defined(CONFIG_HT_SCREEN_TEMPERATURE)
	temperature_io_stop();
#endif

	screen_uart_stop(SCREEN_UART_DISPLAY);
	if (stop_rc != 0) {
		LOG_ERR("Encoder interrupt cleanup failed: %d", stop_rc);
	}
	return rc != 0 ? rc : stop_rc;
}
