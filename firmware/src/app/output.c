/* @brief 独立输出线程执行控制；STOP取消待启动，硬件保护ISR直接静音。 */
#include "app/output.h"
#if defined(CONFIG_HT_OUTPUT_BENCH)
#include "core/bench.h"
static struct bench debug;
#endif
#include "platform/board_io.h"
#include "platform/dds_io.h"
#include "platform/relay_io.h"
#include "config/analog.h"
#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define BOARD_NODE DT_PATH(zephyr_user)
static const struct gpio_dt_spec oc = GPIO_DT_SPEC_GET(BOARD_NODE, oc_gpios);
static const struct gpio_dt_spec ov = GPIO_DT_SPEC_GET(BOARD_NODE, ov_gpios);
static const struct gpio_dt_spec wdi = GPIO_DT_SPEC_GET(BOARD_NODE, wdi_gpios);
static const struct device *const watchdog = DEVICE_DT_GET(DT_NODELABEL(iwdg));
static struct gpio_callback oc_callback, ov_callback;
static struct k_spinlock guard;
static K_SEM_DEFINE(wake, 0, 1);
static struct power control;
static struct output_snapshot published = {.blocked = OUTPUT_REASON_BIT(OUTPUT_REASON_INIT)};
static struct { uint8_t raw; bool connected, okay; int64_t time; } inputs;
static struct {
    bool pending, stop;
    uint32_t frequency, target, epoch;
#if defined(CONFIG_HT_OUTPUT_BENCH)
    bool debug, wave;
    uint8_t relay;
    uint16_t millivolts_pp;
#endif
} request;
static uint32_t stop_epoch, active_epoch;
static bool initialized, shut_down;
static int watchdog_channel;

static int mute(void *ctx) { ARG_UNUSED(ctx); return signal_io_stop(); }
static int select_range(void *ctx, uint8_t value)
{
    ARG_UNUSED(ctx);
    if (value && signal_io_failed()) return -EIO;
    int rc = relay_io_select(value);
    if (value && signal_io_failed()) { (void)relay_io_select(0); return -EIO; }
    return rc;
}
static int start_signal(void *ctx, uint32_t frequency) { ARG_UNUSED(ctx); return signal_io_start(frequency); }
static int level(void *ctx, uint16_t amplitude) { ARG_UNUSED(ctx); return dds_io_set_amplitude(amplitude); }
static int64_t now_ms(void *ctx) { ARG_UNUSED(ctx); return k_uptime_get(); }
static bool cancelled(void *ctx)
{
    ARG_UNUSED(ctx);
    k_spinlock_key_t key = k_spin_lock(&guard);
    bool result = shut_down || active_epoch != stop_epoch;
    k_spin_unlock(&guard, key);
    return result;
}

static void protection(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
    ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);
    signal_io_fault();
}

int output_init(void)
{
    if (initialized) return -EALREADY;
    initialized = true;
    struct power_ops ops = {.mute=mute, .select=select_range, .start=start_signal,
                            .level=level, .now=now_ms, .cancelled=cancelled};
    int rc = power_init(&control, &ops);
#if defined(CONFIG_HT_OUTPUT_BENCH)
    if (rc == 0) rc = bench_init(&debug, &ops);
#endif
    if (rc == 0) rc = signal_io_init();
    /* 上电只清一次旧锁存；之后仍按实际OC/OV电平判断，不自动重试。 */
    if (rc == 0) rc = signal_io_stop();
    if (rc == 0) rc = relay_io_select(0);
    if (rc == 0) rc = relay_io_check();
    if (rc == 0) rc = board_io_clear_protection();
    if (rc == 0) {
        gpio_init_callback(&oc_callback, protection, BIT(oc.pin));
        gpio_init_callback(&ov_callback, protection, BIT(ov.pin));
        rc = gpio_add_callback(oc.port, &oc_callback);
        if (rc == 0) rc = gpio_add_callback(ov.port, &ov_callback);
        if (rc == 0) rc = gpio_pin_interrupt_configure_dt(&oc, GPIO_INT_EDGE_TO_ACTIVE);
        if (rc == 0) rc = gpio_pin_interrupt_configure_dt(&ov, GPIO_INT_EDGE_TO_ACTIVE);
    }
    if (rc == 0) rc = gpio_pin_configure_dt(&wdi, GPIO_OUTPUT_INACTIVE);
    if (rc == 0 && !device_is_ready(watchdog)) rc = -ENODEV;
    if (rc == 0) {
        const struct wdt_timeout_cfg timeout = {.window = {.min=0, .max=1000}, .flags=WDT_FLAG_RESET_SOC};
        watchdog_channel = wdt_install_timeout(watchdog, &timeout);
        rc = watchdog_channel < 0 ? watchdog_channel : wdt_setup(watchdog, WDT_OPT_PAUSE_HALTED_BY_DBG);
    }
    if (rc != 0) {
        signal_io_fault();
        (void)signal_io_stop();
        published.fault = true;
        published.error = rc;
        published.blocked = OUTPUT_REASON_BIT(OUTPUT_REASON_INIT);
        return rc;
    }
    published.ready = true;
    k_sem_give(&wake);
    return 0;
}

void output_inputs(uint8_t raw, bool connected, bool io_ok)
{
    k_spinlock_key_t key = k_spin_lock(&guard);
    inputs.raw = raw;
    inputs.connected = connected;
    inputs.okay = io_ok;
    inputs.time = k_uptime_get();
    if (!io_ok) { request.pending = false; request.stop = true; ++stop_epoch; }
    k_spin_unlock(&guard, key);
}

int output_start(uint32_t frequency, uint32_t target)
{
    if (target == 0 || target > 50000 ||
        (frequency != 2000 && frequency != 5000 && frequency != 8000 && frequency != 10000)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&guard);
    int rc = 0;
    if (!published.ready || !published.available || shut_down) rc = -EACCES;
    else if (published.running || published.switching || request.pending || request.stop) rc = -EBUSY;
    else {
#if defined(CONFIG_HT_OUTPUT_BENCH)
        if (published.debug.busy) { k_spin_unlock(&guard, key); return -EBUSY; }
        request.debug = false;
#endif
        request.pending = true; request.frequency = frequency;
        request.target = target; request.epoch = stop_epoch;
    }
    k_spin_unlock(&guard, key);
    return rc;
}

#if defined(CONFIG_HT_OUTPUT_BENCH)
int output_debug(uint8_t relay, uint32_t frequency, uint16_t millivolts_pp, bool wave)
{
    if (relay > 8 || (frequency != 2000 && frequency != 5000 && frequency != 8000 && frequency != 10000) ||
        (millivolts_pp != 10 && millivolts_pp != 25 && millivolts_pp != 50 && millivolts_pp != 100)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&guard);
    int rc = 0;
    bool available = relay == 0 && wave ? published.debug.dac_available : published.available;
    if (!published.ready || !available || shut_down || published.fault) rc = -EACCES;
    else if (published.active || published.switching || request.pending || request.stop) rc = -EBUSY;
    else {
        request.pending = request.debug = true;
        request.relay = relay; request.frequency = frequency;
        request.millivolts_pp = millivolts_pp; request.wave = wave; request.epoch = stop_epoch;
    }
    k_spin_unlock(&guard, key);
    return rc;
}
#endif

void output_stop(void)
{
    k_spinlock_key_t key = k_spin_lock(&guard);
    request.pending = false;
    request.stop = true;
    ++stop_epoch;
    k_spin_unlock(&guard, key);
}

void output_snapshot(struct output_snapshot *value)
{
    k_spinlock_key_t key = k_spin_lock(&guard);
    *value = published;
    k_spin_unlock(&guard, key);
}

void output_shutdown(void)
{
    k_spinlock_key_t key = k_spin_lock(&guard);
    shut_down = true;
    ++stop_epoch;
    request.pending = false;
    request.stop = true;
    k_spin_unlock(&guard, key);
    signal_io_fault();
}

static void run(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    k_sem_take(&wake, K_FOREVER);
    struct signal_snapshot signal = {0};
    int64_t next_feed = 0;
    bool pin = false;
    for (;;) {
        uint32_t digital = 0;
        int board_rc = board_io_read(&digital);
        int rc = board_rc;
        if (rc == 0) rc = signal_io_poll(&signal);
        int64_t now = k_uptime_get();
        k_spinlock_key_t key = k_spin_lock(&guard);
        bool alive = inputs.okay && now - inputs.time < 100 && !shut_down;
        uint32_t blocked = 0;
#define BLOCK_IF(condition, reason) do { if (condition) blocked |= OUTPUT_REASON_BIT(reason); } while (0)
        BLOCK_IF(shut_down, OUTPUT_REASON_SHUTDOWN);
        BLOCK_IF(!inputs.okay || board_rc != 0, OUTPUT_REASON_INPUT_IO);
        BLOCK_IF(now < inputs.time || now - inputs.time >= 100, OUTPUT_REASON_INPUT_STALE);
        BLOCK_IF(!inputs.connected, OUTPUT_REASON_SCREEN);
        BLOCK_IF(inputs.raw & BIT(6), OUTPUT_REASON_ENABLE);
#if !defined(CONFIG_HT_OUTPUT_BENCH)
        BLOCK_IF(inputs.raw & BIT(7), OUTPUT_REASON_DOOR);
#endif
        bool stop = request.stop;
        bool start = request.pending;
        if (start) active_epoch = request.epoch;
        uint32_t frequency = request.frequency, target = request.target;
#if defined(CONFIG_HT_OUTPUT_BENCH)
        bool debug_start = start && request.debug;
        if (debug_start) start = false;
        uint8_t relay = request.relay;
        uint16_t millivolts_pp = request.millivolts_pp;
        bool wave = request.wave;
#endif
        request.pending = false;
        request.stop = false;
        k_spin_unlock(&guard, key);
        if (board_rc == 0) {
            BLOCK_IF(!(digital & BIT(BOARD_SENSOR)), OUTPUT_REASON_SENSOR);
            BLOCK_IF(!(digital & BIT(BOARD_DC)), OUTPUT_REASON_DC);
#if !defined(CONFIG_HT_OUTPUT_BENCH)
            BLOCK_IF(!(digital & BIT(BOARD_COIL)), OUTPUT_REASON_COIL);
#endif
            BLOCK_IF(digital & BIT(BOARD_OC), OUTPUT_REASON_OC);
            BLOCK_IF(digital & BIT(BOARD_OV), OUTPUT_REASON_OV);
            BLOCK_IF(!(digital & (BIT(BOARD_AC) | BIT(BOARD_BAT))), OUTPUT_REASON_SUPPLY);
        }
        if (signal_io_failed()) rc = -EIO;
        BLOCK_IF(rc != 0 && board_rc == 0, OUTPUT_REASON_SAMPLE_IO);
        BLOCK_IF(now < signal.temperature_ms || now - signal.temperature_ms >= 1000, OUTPUT_REASON_TEMP_STALE);
        for (size_t i = 0; i < 3; ++i) {
            bool open = signal.temperature.samples[i].status == NTC_SATURATED &&
                signal.temperature.samples[i].raw == NTC_ADC_MAX;
            BLOCK_IF(open, OUTPUT_REASON_NTC1_OPEN + i);
            BLOCK_IF(!open && signal.temperature.samples[i].status != NTC_OK, OUTPUT_REASON_NTC1_INVALID + i);
            BLOCK_IF(signal.temperature.samples[i].status == NTC_OK &&
                signal.temperature.samples[i].decicelsius >= HT_OUTPUT_TEMP_LIMIT_DC, OUTPUT_REASON_NTC1_HOT + i);
        }
        bool permitted = blocked == 0;
#if defined(CONFIG_HT_OUTPUT_BENCH)
        /* 仅全释放的DAC试波容许NTC开路，短路与有效过温仍拒绝。 */
        uint32_t dac_blocked = blocked & ~(OUTPUT_REASON_BIT(OUTPUT_REASON_NTC1_OPEN) |
            OUTPUT_REASON_BIT(OUTPUT_REASON_NTC2_OPEN) | OUTPUT_REASON_BIT(OUTPUT_REASON_NTC3_OPEN));
        bool dac_permitted = dac_blocked == 0;
#endif
#undef BLOCK_IF
        if (rc != 0) {
            signal.reading.valid = false;
#if defined(CONFIG_HT_OUTPUT_BENCH)
            if (debug.state != BENCH_IDLE) bench_fail(&debug, POWER_ERROR_SAMPLE);
#endif
            power_fail(&control, POWER_ERROR_SAMPLE);
        }
        /* 仅限制启动准入，输出起始的首窗等待仍由POWER_ZERO管理。 */
        bool sample_ready = signal.reading.valid && now >= signal.reading.time_ms &&
            now - signal.reading.time_ms < 150;
#if defined(CONFIG_HT_OUTPUT_BENCH)
        if (stop) bench_stop(&debug, now);
        else if (debug_start && (relay == 0 && wave ? dac_permitted && debug.output == 0 : permitted) &&
                 sample_ready && control.state == POWER_IDLE)
            (void)bench_start(&debug, relay, frequency, millivolts_pp, wave, now);
        if (debug.state != BENCH_IDLE) start = false;
        bench_poll(&debug, debug.relay == 0 && debug.wave ? dac_permitted && debug.output == 0 : permitted,
                   &signal.reading, k_uptime_get());
#endif
        if (stop) power_stop(&control, now);
        else if (start && permitted && sample_ready)
            (void)power_start(&control, POWER_RANGE_AUTO, frequency, target, now);
        power_poll(&control, permitted, &signal.reading, k_uptime_get());
        uint8_t applied = POWER_RANGE_AUTO;
        if (control.state != POWER_FAULT)
            for (uint8_t i = 0; i < 7; ++i) if (control.output & BIT(i + 1)) applied = i;
        struct output_snapshot next = {
            .ready = true, .available = permitted && sample_ready && control.state != POWER_FAULT,
            .active = control.state != POWER_IDLE && control.state != POWER_FAULT &&
                      control.state != POWER_STOPPING && control.state != POWER_RELEASE,
            .matching = control.matching && control.state != POWER_IDLE && control.state != POWER_FAULT &&
                        control.state != POWER_STOPPING && control.state != POWER_RELEASE,
            .running = control.state == POWER_RUNNING,
            .switching = control.state != POWER_RUNNING && control.state != POWER_IDLE && control.state != POWER_FAULT,
            .fault = control.state == POWER_FAULT, .error = control.error,
            .range = applied,
            .frequency = control.state == POWER_ZERO || control.state == POWER_PROBING ||
                         control.state == POWER_RUNNING ? control.frequency : 0,
            .target_mva = control.target_mva,
            .elapsed_seconds = control.state == POWER_RUNNING ? (uint32_t)((now-control.session_ms)/1000) : 0,
            .blocked = blocked | (sample_ready ? 0 : OUTPUT_REASON_BIT(OUTPUT_REASON_SAMPLE_STALE)) |
                (control.state == POWER_FAULT ? OUTPUT_REASON_BIT(OUTPUT_REASON_FAULT) : 0),
            .signal = signal
        };
#if defined(CONFIG_HT_OUTPUT_BENCH)
        next.debug.busy = debug.state != BENCH_IDLE && debug.state != BENCH_FAULT;
        next.debug.wave = debug.wave && debug.state == BENCH_ON;
        next.debug.trial = debug.wave && next.debug.busy &&
            debug.state != BENCH_STOP && debug.state != BENCH_RELEASE;
        next.debug.coils = debug.output;
        next.debug.blocked = dac_blocked |
            (sample_ready ? 0 : OUTPUT_REASON_BIT(OUTPUT_REASON_SAMPLE_STALE)) |
            (control.state != POWER_IDLE && control.state != POWER_FAULT ? OUTPUT_REASON_BIT(OUTPUT_REASON_BUSY) : 0) |
            (control.state == POWER_FAULT || debug.state == BENCH_FAULT ? OUTPUT_REASON_BIT(OUTPUT_REASON_FAULT) : 0) |
            (debug.output ? OUTPUT_REASON_BIT(OUTPUT_REASON_RELAYS) : 0);
        next.debug.dac_available = next.debug.blocked == 0;
        next.debug.seconds = next.debug.busy && debug.expires > now ?
            (uint32_t)((debug.expires - now + 999) / 1000) : 0;
        if (debug.state == BENCH_FAULT) {
            next.fault = true; next.error = debug.error; next.available = false;
            next.blocked |= OUTPUT_REASON_BIT(OUTPUT_REASON_FAULT);
        }
        bool debug_safe = debug.state != BENCH_FAULT || debug.fault_stopped;
#else
        bool debug_safe = true;
#endif
        key = k_spin_lock(&guard);
        published = next;
        k_spin_unlock(&guard, key);
        if (alive && debug_safe && (control.state != POWER_FAULT || control.fault_stopped) && now >= next_feed) {
            (void)wdt_feed(watchdog, watchdog_channel);
            pin = !pin;
            (void)gpio_pin_set_dt(&wdi, pin);
            next_feed = now + 50;
        }
        k_msleep(1);
    }
}
K_THREAD_DEFINE(output_thread, 3072, run, NULL, NULL, NULL, 2, 0, 0);
