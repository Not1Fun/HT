/* 正式输出分支：测量经面板快照和DGUS编码到达屏幕，硬件边界用固定快照替代。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/app/screen_panel.c"

const struct device mock_gpio = {0};
static int64_t clock_ms = 1000;
static struct output_snapshot measured;
static unsigned int starts;
static char current[VIEW_TEXT_BYTES], voltage[VIEW_TEXT_BYTES];

int64_t k_uptime_get(void) { return clock_ms; }
void k_msleep(int ms) { clock_ms += ms; }
unsigned int irq_lock(void) { return 0; }
void irq_unlock(unsigned int key) { (void)key; }
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
{ (void)port; (void)cb; return 0; }
int gpio_remove_callback(const struct device *port, struct gpio_callback *cb)
{ (void)port; (void)cb; return 0; }
int board_io_read(uint32_t *value) { *value = BIT(BOARD_BAT); return 0; }
int relay_io_check(void) { return 0; }
int relay_io_read_panel(uint8_t *value) { *value = 0x3f; return 0; }
int screen_uart_init(enum screen_uart_port port) { (void)port; return 0; }
int screen_uart_check(enum screen_uart_port port) { (void)port; return 0; }
int screen_uart_read(enum screen_uart_port port, uint8_t *data, size_t capacity)
{ (void)port; (void)data; (void)capacity; return 0; }
int screen_uart_write(enum screen_uart_port port, const uint8_t *data, size_t length)
{ (void)port; (void)data; (void)length; return 0; }
void screen_uart_stop(enum screen_uart_port port) { (void)port; }
int output_init(void) { return 0; }
void output_inputs(uint8_t raw, bool connected, bool io_ok)
{ (void)raw; (void)connected; (void)io_ok; }
int output_start(uint32_t frequency, uint32_t target_mva)
{ (void)frequency; (void)target_mva; ++starts; return 0; }
void output_stop(void) { }
void output_snapshot(struct output_snapshot *value) { *value = measured; }
void output_shutdown(void) { }

static int capture(void *ctx, const uint8_t *data, size_t length)
{
    struct dgus_frame frame;
    (void)ctx;
    assert(dgus_decode(DGUS_CRC_NONE, data, length, &frame) == 0);
    char *text = frame.vp == 0x1100 ? current : frame.vp == 0x1110 ? voltage : NULL;
    if (text) {
        assert(frame.count == VIEW_TEXT_BYTES / 2);
        for (size_t i = 0; i < frame.count; ++i) {
            text[i * 2] = (char)(frame.words[i] >> 8);
            text[i * 2 + 1] = (char)frame.words[i];
        }
    }
    return 0;
}

static struct service setup(void)
{
    struct service s = {.link.connected = true, .digital_valid = true, .digital = BIT(BOARD_BAT)};
    const struct panel_config limits = {7070, 50000};
    assert(panel_init(&s.panel, &limits, PANEL_VA, true, false) == 0);
    event_log_init(&s.log);
    measured = (struct output_snapshot){
        .ready = true, .range = 3, .frequency = 8000, .elapsed_seconds = 37,
        .signal.reading = {.voltage_mv = 12340, .current_ma = 1250, .time_ms = 990, .valid = true}
    };
    return s;
}

static struct view_snapshot render(struct service *s, const char *amps, const char *volts)
{
    struct view_snapshot value;
    snapshot(s, &value);
    memset(current, 0xff, sizeof(current));
    memset(voltage, 0xff, sizeof(voltage));
    assert(view_refresh(&s->view, &value, DGUS_CRC_NONE, capture, NULL) == VIEW_OK);
    assert(strcmp(current, amps) == 0 && strcmp(voltage, volts) == 0);
    assert(starts == 0);
    return value;
}

static void standby(void)
{
    struct service s = setup();
    struct view_snapshot value = render(&s, "1.250", "12.340");
    assert(value.state == VIEW_STANDBY && value.output == VIEW_OUTPUT_UNAVAILABLE);
    assert(value.values[VIEW_CURRENT].valid && value.values[VIEW_VOLTAGE].valid);
    assert(!value.values[VIEW_RANGE].valid && !value.values[VIEW_FREQUENCY].valid);
    assert(!value.values[VIEW_ELAPSED].valid);
    measured.signal.reading.current_ma = 0;
    measured.signal.reading.voltage_mv = 0;
    (void)render(&s, "0.000", "0.000");
}

static void measurement_age(void)
{
    struct service s = setup();
    (void)render(&s, "1.250", "12.340");
    measured.signal.reading.valid = false;
    (void)render(&s, "--", "--");
    measured.signal.reading.valid = true;
    measured.signal.reading.time_ms = clock_ms - 149;
    (void)render(&s, "1.250", "12.340");
    ++clock_ms;
    struct view_snapshot value = render(&s, "--", "--");
    assert(!value.values[VIEW_CURRENT].valid && !value.values[VIEW_VOLTAGE].valid);
    measured.signal.reading.time_ms = clock_ms + 1;
    (void)render(&s, "--", "--");
    measured.signal.reading.time_ms = clock_ms;
    (void)render(&s, "1.250", "12.340");
}

static void output_states(void)
{
    struct service s = setup();
    measured.active = measured.switching = measured.matching = true;
    struct view_snapshot value = render(&s, "1.250", "12.340");
    assert(value.state == VIEW_SWITCHING && value.output == VIEW_OUTPUT_RUNNING);
    assert(value.values[VIEW_RANGE].valid && value.values[VIEW_RANGE].value == 30);
    assert(value.values[VIEW_FREQUENCY].valid && value.values[VIEW_FREQUENCY].value == 8000);
    assert(!value.values[VIEW_ELAPSED].valid);
    measured.switching = measured.matching = false;
    measured.running = true;
    value = render(&s, "1.250", "12.340");
    assert(value.state == VIEW_RUNNING);
    assert(value.values[VIEW_ELAPSED].valid && value.values[VIEW_ELAPSED].value == 37);
    measured.active = measured.running = false;
    value = render(&s, "1.250", "12.340");
    assert(value.state == VIEW_STANDBY && !value.values[VIEW_ELAPSED].valid);
    assert(!value.values[VIEW_RANGE].valid && !value.values[VIEW_FREQUENCY].valid);
    measured.fault = true;
    (void)render(&s, "--", "--");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "standby")) standby();
    else if (!strcmp(argv[1], "measurement_age")) measurement_age();
    else if (!strcmp(argv[1], "output_states")) output_states();
    else assert(!"Unknown test");
    printf("PASS %s\n", argv[1]);
    return 0;
}
