/* @brief 独立输出线程执行控制；STOP取消待启动，硬件保护ISR直接静音。 */
#include "app/output.h"
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
static struct output_snapshot published;
static struct { uint8_t raw; bool connected, okay; int64_t time; } inputs;
static struct { bool pending, stop; uint8_t range; uint32_t frequency, target, epoch; } request;
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
    if (rc == 0) rc = signal_io_init();
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
        (void)signal_io_stop();
        published.fault = true;
        published.error = rc;
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
    else if (published.running || published.switching || request.pending || request.stop) rc = -EBUSY;
    else {
        request.pending = true; request.range = range; request.frequency = frequency;
        request.target = target; request.epoch = stop_epoch;
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

static void run(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    k_sem_take(&wake, K_FOREVER);
    struct signal_snapshot signal = {0};
    int64_t next_feed = 0;
    bool pin = false;
    for (;;) {
        uint32_t digital = 0;
        int rc = board_io_read(&digital);
        if (rc == 0) rc = signal_io_poll(&signal);
        int64_t now = k_uptime_get();
        k_spinlock_key_t key = k_spin_lock(&guard);
        bool alive = inputs.okay && now - inputs.time < 100 && !shut_down;
        bool permitted = alive && inputs.connected && (inputs.raw & 0xc0) == 0;
        bool stop = request.stop;
        bool start = request.pending;
        if (start) active_epoch = request.epoch;
        uint8_t range = request.range;
        uint32_t frequency = request.frequency, target = request.target;
        request.pending = false;
        request.stop = false;
        k_spin_unlock(&guard, key);
        const uint32_t required = BIT(BOARD_SENSOR) | BIT(BOARD_COIL) | BIT(BOARD_DC);
        permitted = permitted && rc == 0 && (digital & required) == required &&
            !(digital & (BIT(BOARD_OC) | BIT(BOARD_OV))) &&
            ((digital & BIT(BOARD_AC)) || (digital & BIT(BOARD_BAT))) &&
            now - signal.temperature_ms < 1000;
        /* 暂用80°C软件保护阈值；硬件安全链独立生效，实机需核对探头安装位置。 */
        for (size_t i = 0; i < 3; ++i) permitted = permitted &&
            signal.temperature.samples[i].status == NTC_OK &&
            signal.temperature.samples[i].decicelsius < HT_OUTPUT_TEMP_LIMIT_DC;
        if (signal_io_failed()) rc = -EIO;
        if (rc != 0) power_fail(&control, POWER_ERROR_SAMPLE);
        if (stop) power_stop(&control, now);
        else if (start && permitted) (void)power_start(&control, range, frequency, target, now);
        power_poll(&control, permitted, &signal.reading, k_uptime_get());
        struct output_snapshot next = {
            .ready = true, .available = permitted && control.state != POWER_FAULT,
            .running = control.state == POWER_RUNNING,
            .switching = control.state != POWER_RUNNING && control.state != POWER_IDLE && control.state != POWER_FAULT,
            .fault = control.state == POWER_FAULT, .error = control.error,
            .range = control.range, .frequency = control.frequency, .target_mva = control.target_mva,
            .elapsed_seconds = control.state == POWER_RUNNING ? (uint32_t)((now-control.started)/1000) : 0,
            .signal = signal
        };
        key = k_spin_lock(&guard);
        published = next;
        k_spin_unlock(&guard, key);
        if (alive && (control.state != POWER_FAULT || control.fault_stopped) && now >= next_feed) {
            (void)wdt_feed(watchdog, watchdog_channel);
            pin = !pin;
            (void)gpio_pin_set_dt(&wdi, pin);
            next_feed = now + 50;
        }
        k_msleep(1);
    }
}
K_THREAD_DEFINE(output_thread, 3072, run, NULL, NULL, NULL, 2, 0, 0);
