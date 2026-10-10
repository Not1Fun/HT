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
    bool pending, stop, clear;
    uint8_t range;
    uint32_t frequency, target, epoch;
#if defined(CONFIG_HT_OUTPUT_BENCH)
    bool debug, wave;
    uint8_t relay;
    uint16_t millivolts_pp;
#endif
} request;
static uint32_t stop_epoch, active_epoch;
static volatile uint32_t protection_epoch;
static bool initialized, shut_down;
static int watchdog_channel;
static struct {
    uint8_t phase;
    uint32_t epoch, protection, sequence, count;
    int result;
    int64_t began, sampled, healthy;
} recovery;

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
    ++protection_epoch;
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

int output_start(uint8_t range, uint32_t frequency, uint32_t target)
{
    if (range >= 7 || target == 0 || target > 50000 ||
        (frequency != 2000 && frequency != 5000 && frequency != 8000 && frequency != 10000)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&guard);
    int rc = 0;
    if (!published.ready || !published.available || shut_down) rc = -EACCES;
    else if (published.running || published.switching || published.clearing || request.clear || request.pending || request.stop) rc = -EBUSY;
    else {
#if defined(CONFIG_HT_OUTPUT_BENCH)
        if (published.debug.busy) { k_spin_unlock(&guard, key); return -EBUSY; }
        request.debug = false;
#endif
        request.pending = true; request.range = range; request.frequency = frequency;
        request.target = target; request.epoch = stop_epoch;
    }
    k_spin_unlock(&guard, key);
    return rc;
}

#if defined(CONFIG_HT_OUTPUT_BENCH)
int output_debug(uint8_t relay, uint32_t frequency, uint16_t millivolts_pp, bool wave)
{
    if (relay > 8 || (frequency != 2000 && frequency != 5000 && frequency != 8000 && frequency != 10000) ||
        !bench_amplitude_valid(relay, millivolts_pp, wave)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&guard);
    int rc = 0;
    bool available = relay == 0 && wave ? published.debug.dac_available : published.available;
    if (!published.ready || !available || shut_down || published.fault) rc = -EACCES;
    else if (published.active || published.switching || published.clearing || request.clear || request.pending || request.stop) rc = -EBUSY;
    else {
        request.pending = request.debug = true;
        request.relay = relay; request.frequency = frequency;
        request.millivolts_pp = millivolts_pp; request.wave = wave; request.epoch = stop_epoch;
    }
    k_spin_unlock(&guard, key);
    return rc;
}
#endif

int output_clear(void)
{
    k_spinlock_key_t key = k_spin_lock(&guard);
    int rc = 0;
    if (!published.ready || shut_down) rc = -EACCES;
    else if (published.active || published.switching || published.clearing || request.stop
#if defined(CONFIG_HT_OUTPUT_BENCH)
             || published.debug.busy
#endif
    ) rc = -EBUSY;
    else {
        request.pending = false;
        request.clear = true;
        request.epoch = ++stop_epoch;
        published.clearing = true;
    }
    k_spin_unlock(&guard, key);
    return rc;
}

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

static void finish_clear(int result)
{
    if (result != 0) {
        int quiet = signal_io_stop();
        int off = relay_io_select(0);
        power_fail(&control, POWER_ERROR_SAMPLE);
        if (off == 0) control.output = 0;
        control.fault_stopped = quiet == 0 && off == 0;
#if defined(CONFIG_HT_OUTPUT_BENCH)
        if (off == 0) debug.output = 0;
        debug.fault_stopped = quiet == 0 && off == 0;
#endif
    }
    recovery.phase = 0;
    recovery.result = result;
    ++recovery.count;
}

static void clear_poll(bool begin, bool stop, uint32_t blocked,
                       const struct signal_snapshot *signal, bool fresh, int64_t now)
{
    if (begin) {
        recovery.phase = 1;
        recovery.epoch = active_epoch;
        recovery.protection = protection_epoch;
        recovery.began = now;
        recovery.healthy = 0;
    }
    if (!recovery.phase) return;
    if (stop || cancelled(NULL)) { finish_clear(-ECANCELED); return; }
    if (recovery.protection != protection_epoch) { finish_clear(-EIO); return; }
    if (begin) {
        bool idle = control.state == POWER_IDLE || control.state == POWER_FAULT;
#if defined(CONFIG_HT_OUTPUT_BENCH)
        idle = idle && (debug.state == BENCH_IDLE || debug.state == BENCH_FAULT);
#endif
        if (!idle) { finish_clear(-EBUSY); return; }
        int quiet = signal_io_stop();
        int off = relay_io_select(0);
        if (quiet != 0 || off != 0 || relay_io_check() != 0) { finish_clear(-EIO); return; }
        control.output = 0;
        control.fault_stopped = true;
#if defined(CONFIG_HT_OUTPUT_BENCH)
        debug.output = 0;
        debug.fault_stopped = true;
#endif
    }
    if (recovery.phase == 1) {
        if (now - recovery.began < 30) return;
        uint32_t digital = 0;
        int rc = board_io_clear_protection();
        if (rc == 0) rc = board_io_read(&digital);
        if (rc == 0 && (digital & (BIT(BOARD_OC) | BIT(BOARD_OV)))) rc = -EACCES;
        if (rc == 0 && !cancelled(NULL)) rc = signal_io_recover();
        else if (rc == 0) rc = -ECANCELED;
        if (rc != 0) { finish_clear(rc); return; }
        recovery.sequence = signal->reading.sequence;
        recovery.sampled = k_uptime_get();
        recovery.phase = 2;
        return;
    }
    if (signal_io_failed()) { finish_clear(-EIO); return; }
    /* 使能可保持关闭；Bench开路NTC只影响整机启动，不妨碍独立DAC故障恢复。 */
    uint32_t required = blocked & ~OUTPUT_REASON_BIT(OUTPUT_REASON_ENABLE);
#if defined(CONFIG_HT_OUTPUT_BENCH)
    required &= ~(OUTPUT_REASON_BIT(OUTPUT_REASON_NTC1_OPEN) |
        OUTPUT_REASON_BIT(OUTPUT_REASON_NTC2_OPEN) | OUTPUT_REASON_BIT(OUTPUT_REASON_NTC3_OPEN));
#endif
    bool healthy = !required && fresh && signal->reading.sequence != recovery.sequence &&
        signal->reading.time_ms >= recovery.sampled && signal->temperature_ms >= recovery.sampled;
    if (!healthy) recovery.healthy = 0;
    else if (!recovery.healthy) recovery.healthy = now;
    if (healthy && now - recovery.healthy >= 100) {
        if (relay_io_check() != 0) { finish_clear(-EIO); return; }
        k_spinlock_key_t key = k_spin_lock(&guard);
        bool valid = !shut_down && recovery.epoch == stop_epoch &&
            recovery.protection == protection_epoch && !signal_io_failed();
        if (valid) {
            struct power_ops ops = control.ops;
            (void)power_init(&control, &ops);
#if defined(CONFIG_HT_OUTPUT_BENCH)
            (void)bench_init(&debug, &ops);
#endif
        }
        k_spin_unlock(&guard, key);
        finish_clear(valid ? 0 : -ECANCELED);
    } else if (now - recovery.began >= 1000) finish_clear(required ? -EACCES : -ETIMEDOUT);
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
        bool clear = request.clear;
        if (start || clear) active_epoch = request.epoch;
        uint8_t range = request.range;
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
        request.clear = false;
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
        bool clearing = clear || recovery.phase != 0;
        if (rc != 0 && !clearing) {
            signal.reading.valid = false;
#if defined(CONFIG_HT_OUTPUT_BENCH)
            if (debug.state != BENCH_IDLE) bench_fail(&debug, POWER_ERROR_SAMPLE);
#endif
            power_fail(&control, POWER_ERROR_SAMPLE);
        }
        /* 仅限制启动准入，输出起始的首窗等待仍由POWER_ZERO管理。 */
        bool sample_ready = signal.reading.valid && now >= signal.reading.time_ms &&
            now - signal.reading.time_ms < 150;
        clear_poll(clear, stop, blocked, &signal, sample_ready, now);
        if (clearing) start = false;
#if defined(CONFIG_HT_OUTPUT_BENCH)
        if (clearing) debug_start = false;
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
            (void)power_start(&control, range, frequency, target, now);
        power_poll(&control, permitted, &signal.reading, k_uptime_get());
        uint8_t applied = POWER_RANGE_AUTO;
        if (control.state != POWER_FAULT)
            for (uint8_t i = 0; i < 7; ++i) if (control.output & BIT(i + 1)) applied = i;
        struct output_snapshot next = {
            .ready = true, .available = !clearing && permitted && sample_ready && control.state != POWER_FAULT,
            .clearing = recovery.phase != 0, .clear_count = recovery.count, .clear_result = recovery.result,
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
        next.debug.dac_available = !clearing && next.debug.blocked == 0;
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
        next.clear_needed = next.fault || (blocked & (OUTPUT_REASON_BIT(OUTPUT_REASON_OC) |
            OUTPUT_REASON_BIT(OUTPUT_REASON_OV))) != 0;
        key = k_spin_lock(&guard);
        next.clearing |= request.clear;
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
