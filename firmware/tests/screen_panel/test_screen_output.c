/* 正式输出分支：测量经面板快照和DGUS编码到达屏幕，硬件边界用固定快照替代。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/app/screen_panel.c"

const struct device mock_gpio = {0};
static int64_t clock_ms = 1000;
static struct output_snapshot measured;
static unsigned int starts, stops;
static unsigned int clear_requests;
int output_clear(void) { ++clear_requests; return 0; }
static uint8_t requested_range;
#if defined(CONFIG_HT_OUTPUT_BENCH)
static unsigned int debug_starts;
static uint8_t debug_relay;
static bool debug_wave;
static uint32_t debug_frequency;
static uint16_t debug_mvpp;
int output_debug(uint8_t relay, uint32_t frequency, uint16_t mvpp, bool wave)
{ debug_frequency=frequency; debug_mvpp=mvpp; ++debug_starts; debug_relay=relay; debug_wave=wave; return 0; }
#endif
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
int output_start(uint8_t range, uint32_t frequency, uint32_t target_mva)
{ requested_range=range; (void)frequency; (void)target_mva; ++starts; return 0; }
void output_stop(void) { ++stops; }
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


static void press(struct service *s, enum panel_key key)
{
    assert(update_keys(s,0x3f,clock_ms)==0); clock_ms+=30;
    assert(update_keys(s,0x3f,clock_ms)==0); clock_ms+=30;
    uint8_t raw=(uint8_t)(0x3f & ~BIT(key));
    assert(update_keys(s,raw,clock_ms)==0); clock_ms+=30;
    assert(update_keys(s,raw,clock_ms)==0); clock_ms+=30;
}
#if defined(CONFIG_HT_OUTPUT_BENCH)
static void debug_keys(void)
{
    struct service s=setup(); s.panel.debug_enabled=true;
    press(&s,PANEL_KEY_LEFT); assert(s.panel.page==PANEL_PAGE_DAC && stops==1);
    press(&s,PANEL_KEY_LEFT); assert(s.panel.page==PANEL_PAGE_DEBUG && stops==2);
    assert(panel_rotate(&s.panel,2)==0); press(&s,PANEL_KEY_ENCODER);
    assert(debug_starts==1 && debug_relay==2 && !debug_wave && starts==0);
    for(unsigned i=0;i<3;++i) press(&s,PANEL_KEY_DOWN);
    assert(panel_rotate(&s.panel,1)==0); press(&s,PANEL_KEY_OK);
    assert(debug_starts==2 && debug_wave);
    measured.debug.trial=true; measured.debug.busy=true;
    update_output(&s,true); assert(s.panel.debug.wave);
    measured.debug.wave=true; measured.debug.coils=2;
    measured.debug.seconds=9; measured.available=true; update_output(&s,true);
    struct view_snapshot v; snapshot(&s,&v);
    assert(v.debug.state==2 && v.debug.coils==2 && v.debug.seconds==9);
    press(&s,PANEL_KEY_OK); assert(stops==3 && debug_starts==2);
    press(&s,PANEL_KEY_RIGHT); assert(stops==4 && s.panel.page==PANEL_PAGE_DAC);
    assert(s.panel.debug.choice[0]==0);
}
static void dac_keys(void)
{
    struct service s=setup(); s.panel.debug_enabled=true;
    press(&s,PANEL_KEY_LEFT);
    assert(s.panel.page==PANEL_PAGE_DAC && stops==1);
    measured.available=false; measured.debug.dac_available=true;
    struct view_snapshot v; snapshot(&s,&v);
    assert(v.debug.state==0 && v.debug.relay==0 && v.debug.field==1);
    assert(v.debug.frequency==2000 && v.debug.millivolts_pp==10);
    s.panel.page=PANEL_PAGE_DEBUG;
    snapshot(&s,&v); assert(v.debug.state==0);
    s.panel.debug.choice[0]=8;
    snapshot(&s,&v); assert(v.debug.state==3);
    s.panel.page=PANEL_PAGE_DAC;
    for (unsigned i=0;i<2;++i) press(&s,PANEL_KEY_DOWN);
    assert(panel_rotate(&s.panel,1)==0); press(&s,PANEL_KEY_ENCODER);
    assert(debug_starts==1 && debug_wave && debug_relay==0);
    assert(debug_frequency==2000 && debug_mvpp==10 && starts==0);
    measured.debug.trial=measured.debug.busy=measured.debug.wave=true;
    measured.debug.seconds=8; update_output(&s,true); snapshot(&s,&v);
    assert(v.debug.state==2 && v.debug.seconds==8 && v.debug.relay==0);
    press(&s,PANEL_KEY_OK); assert(stops==2 && debug_starts==1);
    measured.debug.dac_available=false; snapshot(&s,&v); assert(v.debug.state==3);
    measured.fault=true; snapshot(&s,&v); assert(v.debug.state==4);
    press(&s,PANEL_KEY_RIGHT); assert(stops==3 && s.panel.page==PANEL_PAGE_STATUS);
    assert(s.panel.debug.choice[0]==0);
}

#endif

static void manual_selection(void)
{
    struct service s=setup(); measured.available=true;
    s.panel.apparent_mva=1000; update_output(&s,true);
    press(&s,PANEL_KEY_RIGHT); assert(s.panel.field==PANEL_RANGE);
    assert(panel_rotate(&s.panel,2)==0);
    struct view_snapshot v; snapshot(&s,&v);
    assert(v.values[VIEW_RANGE_CHOICE].value==10 && !v.values[VIEW_RANGE].valid);
    press(&s,PANEL_KEY_ENCODER); assert(stops==1 && starts==0 && s.panel.range==2);
    for(unsigned int i=0;i<3;++i) press(&s,PANEL_KEY_DOWN);
    assert(s.panel.field==PANEL_OUTPUT && panel_rotate(&s.panel,1)==0);
    press(&s,PANEL_KEY_OK); assert(starts==1 && requested_range==2);
    measured.active=measured.running=true; measured.range=2; update_output(&s,true);
    for(unsigned int i=0;i<3;++i) press(&s,PANEL_KEY_UP);
    assert(panel_rotate(&s.panel,1)==0); snapshot(&s,&v);
    assert(v.values[VIEW_RANGE].value==10 && v.values[VIEW_RANGE_CHOICE].value==30);
    press(&s,PANEL_KEY_OK); assert(stops==2 && starts==1 && s.panel.range==3);
}

static void reasons(void)
{
    struct service s = setup(); s.panel.page = PANEL_PAGE_SETTINGS;
    measured.blocked = OUTPUT_REASON_BIT(OUTPUT_REASON_OC) | OUTPUT_REASON_BIT(OUTPUT_REASON_OV);
    struct view_snapshot v;
    clock_ms = 0; snapshot(&s, &v); assert(v.reason == OUTPUT_REASON_OC);
    clock_ms = 2000; snapshot(&s, &v); assert(v.reason == OUTPUT_REASON_OV);
    clock_ms = 4000; snapshot(&s, &v); assert(v.reason == OUTPUT_REASON_TARGET);
    s.panel.apparent_mva = 1000; measured.blocked = 0;
    snapshot(&s, &v); assert(v.reason == OUTPUT_REASON_NONE);
    measured.blocked = OUTPUT_REASON_BIT(OUTPUT_REASON_FAULT); measured.error = POWER_ERROR_MATCH;
    snapshot(&s, &v); assert(v.reason == OUTPUT_REASON_COUNT + POWER_ERROR_MATCH - 1);
#if defined(CONFIG_HT_OUTPUT_BENCH)
    s.panel.page = PANEL_PAGE_DAC; s.panel.debug.field = 1;
    measured.blocked = OUTPUT_REASON_BIT(OUTPUT_REASON_NTC2_OPEN);
    measured.debug.blocked = 0; measured.debug.dac_available = true;
    snapshot(&s, &v); assert(v.reason == OUTPUT_REASON_NONE);
    measured.debug.blocked = OUTPUT_REASON_BIT(OUTPUT_REASON_OC);
    snapshot(&s, &v); assert(v.reason == OUTPUT_REASON_OC);
#endif
}
static void clear_controls(void)
{
    struct service s = setup(); measured.fault = measured.clear_needed = true;
    measured.error = POWER_ERROR_OPEN; update_output(&s, true);
    assert(s.dds_failed && event_log_count(&s.log) == 1);
    press(&s, PANEL_KEY_OK); assert(s.panel.field == PANEL_OUTPUT);
    press(&s, PANEL_KEY_ENCODER); assert(clear_requests == 1 && starts == 0 && s.panel.clearing);
    measured.clearing = true; update_output(&s, true);
    struct view_snapshot v; snapshot(&s, &v);
    assert(v.output == VIEW_OUTPUT_CLEARING && v.reason == VIEW_REASON_CLEARING);
    press(&s, PANEL_KEY_OK); assert(clear_requests == 1 && starts == 0);
    measured.clearing = measured.clear_needed = measured.fault = false;
    measured.available = true; measured.clear_count = 1; s.panel.apparent_mva = 1000;
    update_output(&s, true); assert(!s.dds_failed && s.panel.draft_index == 0);
    press(&s, PANEL_KEY_OK); assert(starts == 0 && stops == 1);
    struct event_entry entry; assert(event_log_get(&s.log, 0, &entry) == 0 && entry.kind == EVENT_FAULT_CLEAR);
    measured.fault = measured.clear_needed = true; update_output(&s, true);
    assert(event_log_get(&s.log, 0, &entry) == 0 && entry.kind == EVENT_DDS_FAILED && entry.value == POWER_ERROR_OPEN);
    measured.clear_count = 2; measured.clear_result = -EIO; update_output(&s, true);
    assert(event_log_get(&s.log, 0, &entry) == 0 && entry.kind == EVENT_FAULT_CLEAR_FAILED);
    assert(event_log_count(&s.log) == 4 && starts == 0);
#if defined(CONFIG_HT_OUTPUT_BENCH)
    s.panel.debug_enabled = true; s.panel.page = PANEL_PAGE_DAC; s.panel.debug.field = 3;
    press(&s, PANEL_KEY_OK); assert(clear_requests == 2 && debug_starts == 0);
#endif
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "clear_controls")) { clear_controls(); return 0; }
    if (!strcmp(argv[1], "manual_selection")) { manual_selection(); return 0; }
    if (!strcmp(argv[1], "reasons")) { reasons(); return 0; }
    if (!strcmp(argv[1], "standby")) standby();
    else if (!strcmp(argv[1], "measurement_age")) measurement_age();
    else if (!strcmp(argv[1], "output_states")) output_states();
#if defined(CONFIG_HT_OUTPUT_BENCH)
    else if (!strcmp(argv[1], "debug_keys")) debug_keys();
    else if (!strcmp(argv[1], "dac_keys")) dac_keys();
#endif
    else assert(!"Unknown test");
    printf("PASS %s\n", argv[1]);
    return 0;
}
