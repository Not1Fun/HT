/* @brief 生产screen_panel与dds_bench共同执行，只替代时钟、Zephyr及硬件边界。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/app/screen_panel.c"
#include "platform/dds_io.h"
#include <zephyr/shell/shell.h>

const struct device mock_gpio = {0};
static int64_t clock_ms, exit_at = -1;
static int io_error, write_error, configure_error;
static uint8_t panel_raw = 0xff;
static uint32_t digital_bits = BIT(BOARD_BAT);
static bool auto_reply, rx_pending, bad_start;
static unsigned int gpio_registered, uart_stops, temperature_stops;
static unsigned int starts, stops;
static struct dds_snapshot hardware;
static uint8_t sent[128][DGUS_MAX_FRAME];
static size_t sent_lengths[128], sent_count;

int64_t k_uptime_get(void) { return clock_ms; }
void k_msleep(int ms)
{
    clock_ms += ms;
    if (exit_at >= 0 && clock_ms >= exit_at) io_error = -ECANCELED;
}
unsigned int irq_lock(void) { return 0; }
void irq_unlock(unsigned int key) { (void)key; }
k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
    assert(lock->held == 0);
    lock->held = 1;
    return 0;
}
void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
    (void)key;
    assert(lock->held == 1);
    lock->held = 0;
}
bool gpio_is_ready_dt(const struct gpio_dt_spec *pin) { (void)pin; return true; }
int gpio_port_get_raw(const struct device *port, gpio_port_value_t *value)
{ (void)port; *value = BIT(6) | BIT(7); return 0; }
int gpio_pin_configure_dt(const struct gpio_dt_spec *pin, int flags)
{ (void)pin; (void)flags; return 0; }
int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *pin, int flags)
{ (void)pin; (void)flags; return 0; }
void gpio_init_callback(struct gpio_callback *cb,
    void (*fn)(const struct device *, struct gpio_callback *, gpio_port_pins_t), uint32_t mask)
{ (void)cb; (void)fn; (void)mask; }
int gpio_add_callback(const struct device *port, struct gpio_callback *cb)
{ (void)port; (void)cb; gpio_registered = 1; return 0; }
int gpio_remove_callback(const struct device *port, struct gpio_callback *cb)
{ (void)port; (void)cb; gpio_registered = 0; return 0; }
int board_io_read(uint32_t *value) { *value = digital_bits; return io_error; }
int relay_io_check(void) { return io_error; }
int relay_io_read_panel(uint8_t *value) { *value = panel_raw; return io_error; }
int screen_uart_init(enum screen_uart_port port) { (void)port; return 0; }
int screen_uart_check(enum screen_uart_port port) { (void)port; return 0; }
int screen_uart_read(enum screen_uart_port port, uint8_t *data, size_t capacity)
{
    const uint8_t reply[] = {0x5a, 0xa5, 6, 0x83, 0, 0x0f, 1, 0x65, 0x22};
    (void)port;
    if (!rx_pending) return 0;
    assert(capacity >= sizeof(reply));
    memcpy(data, reply, sizeof(reply));
    rx_pending = false;
    return (int)sizeof(reply);
}
int screen_uart_write(enum screen_uart_port port, const uint8_t *data, size_t length)
{
    (void)port;
    if (write_error) return write_error;
    assert(sent_count < ARRAY_SIZE(sent) && length <= DGUS_MAX_FRAME);
    memcpy(sent[sent_count], data, length);
    sent_lengths[sent_count++] = length;
    if (auto_reply && length > 3 && data[3] == 0x83) rx_pending = true;
    return 0;
}
void screen_uart_stop(enum screen_uart_port port) { (void)port; ++uart_stops; }
int temperature_io_init(void) { return 0; }
int temperature_io_read(struct temperature_snapshot *out)
{
    for (size_t i = 0; i < TEMPERATURE_CHANNEL_COUNT; ++i)
        out->samples[i] = (struct temperature_sample){.raw = 1000, .decicelsius = 250, .status = NTC_OK};
    return 0;
}
void temperature_io_stop(void) { ++temperature_stops; }
void shell_print(const struct shell *shell, const char *format, ...) { (void)shell; (void)format; }
void shell_error(const struct shell *shell, const char *format, ...) { (void)shell; (void)format; }
int dds_io_init(void) { hardware.ready = true; return 0; }
int dds_io_configure(uint32_t frequency, uint16_t amplitude)
{
    if (configure_error) return configure_error;
    hardware.frequency_hz = frequency;
    hardware.amplitude = amplitude;
    return 0;
}
int dds_io_start(void) { ++starts; hardware.running = !bad_start; return 0; }
int dds_io_stop(void) { ++stops; hardware.running = false; return 0; }
int dds_io_check(void) { return 0; }
int dds_io_snapshot(struct dds_snapshot *out) { *out = hardware; return 0; }
void dds_io_fault_stop(void)
{
    hardware.running = false;
    hardware.ready = false;
    hardware.fault = DDS_FAULT_EXTERNAL;
}

static struct dds_bench_snapshot bench_state(void)
{
    struct dds_bench_snapshot state;
    assert(dds_bench_snapshot(&state) == 0);
    return state;
}
static struct service ready(void)
{
    struct service s = {.link.crc = DGUS_CRC_NONE};
    const struct panel_config limits = {7070, 50000};
    assert(panel_init(&s.panel, &limits, PANEL_CC, false, false) == 0);
    event_log_init(&s.log);
    update_digital(&s, BIT(BOARD_BAT));
    s.link.connected = true;
    s.temperature_active = true;
    update_temperature(&s);
    assert(dds_bench_init() == 0);
    s.dds_ready = true;
    update_dds(&s, true);
    assert(s.panel.output_available && !bench_state().running);
    return s;
}
static void healthy_poll(struct service *s)
{
    s->last_temperature = clock_ms;
    update_dds(s, true);
}
static void press_mask(struct service *s, uint8_t mask)
{
    assert(update_keys(s, 0xff, clock_ms) == 0);
    clock_ms += 20;
    assert(update_keys(s, 0xff, clock_ms) == 0);
    uint8_t raw = (uint8_t)(0xffu & ~mask);
    clock_ms += 10;
    assert(update_keys(s, raw, clock_ms) == 0);
    clock_ms += 20;
    assert(update_keys(s, raw, clock_ms) == 0);
    clock_ms += 10;
    assert(update_keys(s, 0xff, clock_ms) == 0);
    clock_ms += 20;
    assert(update_keys(s, 0xff, clock_ms) == 0);
}
static void press(struct service *s, enum panel_key key) { press_mask(s, (uint8_t)BIT(key)); }
static void output_page(struct service *s)
{
    if (s->panel.page == PANEL_PAGE_STATUS) press(s, PANEL_KEY_RIGHT);
    assert(s->panel.page == PANEL_PAGE_SETTINGS);
    while (s->panel.field != PANEL_OUTPUT) press(s, PANEL_KEY_DOWN);
}
static void queue_start(struct service *s)
{
    output_page(s);
    healthy_poll(s);
    assert(panel_rotate(&s->panel, 1) == 0);
    press(s, PANEL_KEY_OK);
}
static void start(struct service *s)
{
    queue_start(s);
    healthy_poll(s);
    assert(hardware.running && bench_state().running && s->panel.output_running);
}
static size_t events(const struct service *s, enum event_kind kind)
{
    size_t found = 0;
    for (size_t i = 0; i < event_log_count(&s->log); ++i) {
        struct event_entry entry;
        assert(event_log_get(&s->log, i, &entry) == 0);
        if (entry.kind == kind) ++found;
    }
    return found;
}
static void stopped_view(struct service *s)
{
    struct view_snapshot value;
    snapshot(s, &value);
    assert(!hardware.running && !bench_state().running);
    assert(value.state != VIEW_RUNNING && value.output != VIEW_OUTPUT_RUNNING);
    for (int field = VIEW_CURRENT; field <= VIEW_ELAPSED; ++field)
        assert(!value.values[field].valid);
    assert(s->panel.current_ma == 0 && s->panel.apparent_mva <= 50000);
}

static void startup(void)
{
    auto_reply = true;
    exit_at = 550;
    assert(screen_panel_run() == -ECANCELED);
    assert(starts == 0 && !hardware.running && !gpio_registered);
    assert(temperature_stops == 1 && uart_stops == 1);
    bool saw_icons = false;
    for (size_t i = 0; i < sent_count; ++i) {
        struct dgus_frame frame;
        assert(dgus_decode(DGUS_CRC_NONE, sent[i], sent_lengths[i], &frame) == 0);
        if (frame.kind == DGUS_WRITE && frame.vp == 0x1000) {
            assert(frame.count == 5 && frame.words[0] == VIEW_STANDBY);
            assert(frame.words[4] == VIEW_OUTPUT_OFF);
            saw_icons = true;
        }
    }
    assert(saw_icons);
}
static void four_fields(void)
{
    struct service s = ready();
    press(&s, PANEL_KEY_RIGHT);
    press(&s, PANEL_KEY_UP);
    assert(s.panel.field == PANEL_RANGE);
    press(&s, PANEL_KEY_DOWN);
    assert(s.panel.field == PANEL_FREQUENCY);
    press(&s, PANEL_KEY_DOWN);
    assert(s.panel.field == PANEL_POWER);
    press(&s, PANEL_KEY_DOWN);
    assert(s.panel.field == PANEL_OUTPUT);
    press(&s, PANEL_KEY_DOWN);
    assert(s.panel.field == PANEL_OUTPUT);
    assert(panel_rotate(&s.panel, INT32_MAX) == 0 && s.panel.draft_index == 1);
    assert(panel_rotate(&s.panel, 1) == 0 && s.panel.draft_index == 1);
    assert(starts == 0);
    press(&s, PANEL_KEY_LEFT);
    press(&s, PANEL_KEY_RIGHT);
    assert(s.panel.draft_index == 0 && !bench_state().running);
    press(&s, PANEL_KEY_OK);
    healthy_poll(&s);
    stopped_view(&s);
}
static void start_parameters(void)
{
    struct service s = ready();
    press(&s, PANEL_KEY_RIGHT);
    press(&s, PANEL_KEY_DOWN);
    assert(panel_rotate(&s.panel, 3) == 0);
    press(&s, PANEL_KEY_OK);
    healthy_poll(&s);
    assert(s.panel.frequency_hz == 10000);
    queue_start(&s);
    struct view_snapshot value;
    snapshot(&s, &value);
    assert(starts == 0 && value.state == VIEW_STANDBY && value.output == VIEW_OUTPUT_ARMED);
    assert(!value.values[VIEW_FREQUENCY].valid && !value.values[VIEW_ELAPSED].valid);
    healthy_poll(&s);
    struct dds_bench_snapshot output = bench_state();
    assert(output.running && output.frequency_hz == 10000);
    assert(output.requested_mvpp == 100 && output.duration_seconds == 60 && output.amplitude_code == 70);
    snapshot(&s, &value);
    assert(value.state == VIEW_RUNNING && value.output == VIEW_OUTPUT_RUNNING);
    assert(value.values[VIEW_FREQUENCY].valid && value.values[VIEW_FREQUENCY].value == 10000);
    assert(value.values[VIEW_ELAPSED].valid && value.values[VIEW_ELAPSED].value == 0);
    assert(!value.values[VIEW_CURRENT].valid && !value.values[VIEW_VOLTAGE].valid && !value.values[VIEW_RANGE].valid);
    assert(events(&s, EVENT_DDS_START) == 1);
    healthy_poll(&s);
    assert(events(&s, EVENT_DDS_START) == 1);
}
static void stop_confirm(void)
{
    struct service s = ready();
    start(&s);
    press(&s, PANEL_KEY_ENCODER);
    healthy_poll(&s);
    stopped_view(&s);
    assert(events(&s, EVENT_DDS_STOP) == 1 && s.panel.draft_index == 0);
    press(&s, PANEL_KEY_OK);
    healthy_poll(&s);
    assert(starts == 1);
    stopped_view(&s);
}
static void request_confirm(bool frequency)
{
    struct service s = ready();
    start(&s);
    enum panel_field target = frequency ? PANEL_FREQUENCY : PANEL_RANGE;
    while (s.panel.field != target) press(&s, PANEL_KEY_UP);
    assert(panel_rotate(&s.panel, 1) == 0);
    press(&s, PANEL_KEY_OK);
    healthy_poll(&s);
    stopped_view(&s);
    assert(frequency ? s.panel.frequency_hz == 5000 : s.panel.range_index == 1);
    assert(events(&s, EVENT_DDS_STOP) == 1);
    output_page(&s);
    assert(s.panel.draft_index == 0);
    press(&s, PANEL_KEY_OK);
    healthy_poll(&s);
    assert(starts == 1);
    stopped_view(&s);
}
static void power_confirm(void)
{
    struct service s = ready();
    start(&s);
    press(&s, PANEL_KEY_UP);
    assert(s.panel.field == PANEL_POWER);
    assert(panel_rotate(&s.panel, 17) == 0);
    struct view_snapshot value;
    snapshot(&s, &value);
    assert(s.panel.apparent_mva == 0 && value.editing);
    assert(value.values[VIEW_POWER_CHOICE].valid && value.values[VIEW_POWER_CHOICE].value == 17000);
    press(&s, PANEL_KEY_OK);
    healthy_poll(&s);
    stopped_view(&s);
    snapshot(&s, &value);
    assert(s.panel.apparent_mva == 17000 && !value.editing);
    assert(value.values[VIEW_POWER_CHOICE].value == 17000);
    assert(events(&s, EVENT_DDS_STOP) == 1);
    output_page(&s);
    assert(s.panel.draft_index == 0);
    press(&s, PANEL_KEY_OK);
    healthy_poll(&s);
    stopped_view(&s);
    assert(starts == 1 && s.panel.apparent_mva == 17000);
}
static void power_cancel(void)
{
    struct service s = ready();
    press(&s, PANEL_KEY_RIGHT);
    press(&s, PANEL_KEY_DOWN);
    press(&s, PANEL_KEY_DOWN);
    assert(s.panel.field == PANEL_POWER);
    assert(panel_rotate(&s.panel, 25) == 0);
    struct view_snapshot value;
    snapshot(&s, &value);
    assert(value.values[VIEW_POWER_CHOICE].value == 25000 && s.panel.apparent_mva == 0);
    press(&s, PANEL_KEY_UP);
    snapshot(&s, &value);
    assert(s.panel.field == PANEL_FREQUENCY && value.values[VIEW_POWER_CHOICE].value == 0);
    press(&s, PANEL_KEY_DOWN);
    assert(s.panel.draft_index == 0);
    assert(panel_rotate(&s.panel, 50) == 0);
    press(&s, PANEL_KEY_RIGHT);
    press(&s, PANEL_KEY_LEFT);
    assert(s.panel.draft_index == 0 && s.panel.apparent_mva == 0);
    assert(panel_rotate(&s.panel, 9) == 0);
    press(&s, PANEL_KEY_ENCODER);
    healthy_poll(&s);
    assert(s.panel.apparent_mva == 9000);
    assert(panel_rotate(&s.panel, 8) == 0);
    press(&s, PANEL_KEY_LEFT);
    press(&s, PANEL_KEY_RIGHT);
    snapshot(&s, &value);
    assert(s.panel.draft_index == 9 && s.panel.apparent_mva == 9000 && !value.editing);
    assert(value.values[VIEW_POWER_CHOICE].value == 9000 && starts == 0);
}
static void power_bounds(void)
{
    struct service s = ready();
    output_page(&s);
    press(&s, PANEL_KEY_UP);
    assert(s.panel.field == PANEL_POWER);
    assert(panel_rotate(&s.panel, INT32_MAX) == 0 && s.panel.draft_index == 50);
    assert(panel_rotate(&s.panel, 1) == 0 && s.panel.draft_index == 50);
    press(&s, PANEL_KEY_OK);
    healthy_poll(&s);
    struct view_snapshot value;
    snapshot(&s, &value);
    assert(s.panel.apparent_mva == 50000 && value.values[VIEW_POWER_CHOICE].value == 50000);
    assert(panel_rotate(&s.panel, INT32_MIN) == 0 && s.panel.draft_index == 0);
    assert(panel_rotate(&s.panel, -1) == 0 && s.panel.draft_index == 0);
    press(&s, PANEL_KEY_ENCODER);
    healthy_poll(&s);
    snapshot(&s, &value);
    assert(s.panel.apparent_mva == 0 && value.values[VIEW_POWER_CHOICE].value == 0 && !value.editing);
    stopped_view(&s);
    assert(starts == 0);
}
static void stop_navigation(enum panel_key navigation)
{
    struct service s = ready();
    start(&s);
    press(&s, PANEL_KEY_LEFT);
    assert(s.panel.page == PANEL_PAGE_STATUS);
    press_mask(&s, (uint8_t)(BIT(PANEL_KEY_DOWN) | BIT(navigation)));
    healthy_poll(&s);
    stopped_view(&s);
    assert(s.panel.page == (navigation == PANEL_KEY_RIGHT ? PANEL_PAGE_SETTINGS : PANEL_PAGE_LOG));
    if (s.panel.page == PANEL_PAGE_LOG) press(&s, PANEL_KEY_RIGHT);
    else press(&s, PANEL_KEY_LEFT);
    assert(s.panel.page == PANEL_PAGE_STATUS);
    assert(dds_bench_request_start(8000, 100, 60) == 0);
    press_mask(&s, (uint8_t)(BIT(PANEL_KEY_DOWN) | BIT(navigation)));
    healthy_poll(&s);
    assert(starts == 1);
    stopped_view(&s);
}
static void permission(struct service *s, const char *kind, bool deny)
{
    s->digital_valid = true;
    s->digital = BIT(BOARD_BAT);
    s->link.connected = true;
    s->link.pending = false;
    s->temperature_active = true;
    s->temperature_seen = true;
    s->last_temperature = clock_ms;
    if (deny) {
        if (!strcmp(kind, "inputs")) s->digital_valid = false;
        else if (!strcmp(kind, "oc")) s->digital |= BIT(BOARD_OC);
        else if (!strcmp(kind, "ov")) s->digital |= BIT(BOARD_OV);
        else if (!strcmp(kind, "screen")) s->link.connected = false;
        else if (!strcmp(kind, "deadline")) { s->link.pending = true; s->link.deadline = clock_ms; }
        else if (!strcmp(kind, "temperature_inactive")) s->temperature_active = false;
        else if (!strcmp(kind, "temperature_unseen")) s->temperature_seen = false;
        else if (!strcmp(kind, "temperature_stale")) s->last_temperature = clock_ms - 1000;
        else assert(!strcmp(kind, "io"));
    }
    update_dds(s, !(deny && !strcmp(kind, "io")));
}
static void revoke_permission(const char *kind)
{
    struct service s = ready();
    start(&s);
    permission(&s, kind, true);
    stopped_view(&s);
    assert(!s.panel.output_available && !bench_state().permitted);
    permission(&s, kind, false);
    assert(s.panel.output_available && s.panel.draft_index == 0);
    stopped_view(&s);
    queue_start(&s);
    permission(&s, kind, true);
    assert(starts == 1);
    permission(&s, kind, false);
    healthy_poll(&s);
    stopped_view(&s);
    assert(starts == 1);
}
static void display_error(void)
{
    struct service s = ready();
    start(&s);
    write_error = -EIO;
    s.last_temperature = clock_ms;
    const uint8_t data[] = {0x5a, 0xa5, 5, 0x82, 0x10, 0, 0, 0};
    assert(display_send(&s, data, sizeof(data)) == -EIO);
    stopped_view(&s);
    assert(!s.panel.output_available && !bench_state().permitted);
}
static void failed_start(bool false_running)
{
    struct service s = ready();
    if (false_running) bad_start = true;
    else configure_error = -EIO;
    queue_start(&s);
    healthy_poll(&s);
    stopped_view(&s);
    struct view_snapshot value;
    snapshot(&s, &value);
    assert(value.state == VIEW_FAULT && value.output == VIEW_OUTPUT_FAULT);
    assert(events(&s, EVENT_DDS_FAILED) == 1 && events(&s, EVENT_DDS_START) == 0);
    healthy_poll(&s);
    assert(events(&s, EVENT_DDS_FAILED) == 1);
}
static void expiry(void)
{
    struct service s = ready();
    start(&s);
    int64_t began = clock_ms;
    clock_ms += 29999;
    press(&s, PANEL_KEY_RIGHT);
    press(&s, PANEL_KEY_RIGHT);
    assert(s.panel.page == PANEL_PAGE_STATUS);
    healthy_poll(&s);
    assert(bench_state().running);
    clock_ms = began + 59999;
    healthy_poll(&s);
    assert(bench_state().running);
    clock_ms = began + 60000;
    healthy_poll(&s);
    stopped_view(&s);
    assert(events(&s, EVENT_DDS_STOP) == 1);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *name = argv[1];
    if (!strcmp(name, "startup")) startup();
    else if (!strcmp(name, "four_fields")) four_fields();
    else if (!strcmp(name, "start_parameters")) start_parameters();
    else if (!strcmp(name, "stop_confirm")) stop_confirm();
    else if (!strcmp(name, "range_confirm")) request_confirm(false);
    else if (!strcmp(name, "frequency_confirm")) request_confirm(true);
    else if (!strcmp(name, "power_confirm")) power_confirm();
    else if (!strcmp(name, "power_cancel")) power_cancel();
    else if (!strcmp(name, "power_bounds")) power_bounds();
    else if (!strcmp(name, "stop_navigation_right")) stop_navigation(PANEL_KEY_RIGHT);
    else if (!strcmp(name, "stop_navigation_left")) stop_navigation(PANEL_KEY_LEFT);
    else if (!strncmp(name, "permission_", 11)) revoke_permission(name + 11);
    else if (!strcmp(name, "display_error")) display_error();
    else if (!strcmp(name, "configure_failure")) failed_start(false);
    else if (!strcmp(name, "false_running")) failed_start(true);
    else if (!strcmp(name, "expiry")) expiry();
    else assert(!"Unknown test");
    printf("PASS %s\n", name);
    return 0;
}
