/* @brief LPUART1 115200 8N1；硬件DE控制PB12，扫描0x80..0x83。 */
#include "platform/drs_io.h"
#include <errno.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(drs);
static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(lpuart1));
static struct k_spinlock lock;
static struct drs_snapshot published;
static uint8_t tx[8];
static volatile size_t sent;
static atomic_t rx_error;
K_MSGQ_DEFINE(drs_rx_queue, 1, 32, 1);

static void interrupt(const struct device *dev, void *ctx)
{
    ARG_UNUSED(ctx);
    if (uart_irq_update(dev) < 0) { atomic_set(&rx_error, 1); return; }
    if (uart_err_check(dev) != 0) atomic_set(&rx_error, 1);
    if (uart_irq_tx_ready(dev)) {
        if (sent < sizeof(tx)) {
            int n = uart_fifo_fill(dev, tx+sent, (int)(sizeof(tx)-sent));
            if (n > 0 && (size_t)n <= sizeof(tx)-sent) sent += (size_t)n;
            else { atomic_set(&rx_error, 1); uart_irq_tx_disable(dev); }
        }
        if (sent == sizeof(tx)) uart_irq_tx_disable(dev);
    }
    if (uart_irq_rx_ready(dev)) {
        uint8_t bytes[16];
        for (unsigned int n = 0; n < 4; ++n) {
            int got = uart_fifo_read(dev, bytes, sizeof(bytes));
            if (got <= 0) { if (got < 0) atomic_set(&rx_error, 1); break; }
            for (int i = 0; i < got; ++i)
                if (k_msgq_put(&drs_rx_queue, &bytes[i], K_NO_WAIT) != 0) atomic_set(&rx_error, 1);
        }
    }
}
static int read_register(uint8_t address, enum drs_register reg, uint16_t *value)
{
    uart_irq_tx_disable(uart);
    /* 上次事务后已空闲100ms，清掉迟到/残留字节再开始新请求。 */
    k_msgq_purge(&drs_rx_queue);
    atomic_clear(&rx_error);
    sent = 0;
    if (drs_request(address, reg, tx) != 0) return -EINVAL;
    uint8_t frame[7]; size_t used = 0;
    int result = -ETIMEDOUT;
    int64_t end = k_uptime_get()+50;
    uart_irq_tx_enable(uart);
    while (k_uptime_get() < end) {
        uint8_t byte;
        if (atomic_get(&rx_error)) { result = -EIO; break; }
        if (k_msgq_get(&drs_rx_queue, &byte, K_MSEC(1)) != 0) continue;
        if (used == sizeof(frame)) { memmove(frame, frame+1, --used); }
        frame[used++] = byte;
        uint16_t data;
        int rc = 0;
        if (used >= 5) rc = drs_reply(address, reg, frame+used-5, 5, &data);
        if (rc >= 0 && used == sizeof(frame)) rc = drs_reply(address, reg, frame, used, &data);
        if (rc < 0) { result = -EPROTO; break; }
        if (rc == 1) {
            if (sent != sizeof(tx) || !uart_irq_tx_complete(uart) || atomic_get(&rx_error)) {
                result = -EIO; break;
            }
            *value = data; result = 0; break;
        }
    }
    uart_irq_tx_disable(uart);
    return result;
}
void drs_io_snapshot(struct drs_snapshot *state)
{
    if (!state) return;
    k_spinlock_key_t key = k_spin_lock(&lock);
    *state = published;
    k_spin_unlock(&lock, key);
    int64_t now = k_uptime_get();
    if (now < state->time_ms || now-state->time_ms >= 2000) state->valid = false;
}
static void run(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    if (!device_is_ready(uart) || uart_irq_callback_user_data_set(uart, interrupt, NULL) != 0) {
        k_spinlock_key_t key = k_spin_lock(&lock);
        published.error = -ENODEV;
        k_spin_unlock(&lock, key);
        LOG_ERR("DRS UART unavailable");
        return;
    }
    uart_irq_rx_enable(uart);
    uart_irq_err_enable(uart);
    uint8_t address = 0x83;
    unsigned int failures = 0;
    for (;;) {
        struct drs_snapshot next = {.address=address};
        int rc = 0;
        for (enum drs_register reg = 0; reg < DRS_REGISTER_COUNT; ++reg) {
            uint16_t value;
            rc = read_register(address, reg, &value);
            if (rc == 0) drs_store(&next, reg, value);
            k_msleep(100); /* 请求间隔及静默均大于明纬手册最小值。 */
            if (rc != 0) break;
        }
        k_spinlock_key_t key = k_spin_lock(&lock);
        bool was_valid = published.valid;
        bool changed = !was_valid || next.fault != published.fault ||
            next.charge != published.charge || next.system != published.system;
        if (rc == 0) {
            next.valid = true; next.time_ms = k_uptime_get();
            next.sequence = published.sequence+1;
            published = next;
        } else { published.valid = false; published.error = rc; }
        k_spin_unlock(&lock, key);
        if (rc == 0) {
            failures = 0;
            if (changed) LOG_INF("DRS 0x%02x fault=%04x charge=%04x system=%04x",
                address, next.fault, next.charge, next.system);
        } else {
            if (was_valid) LOG_WRN("DRS lost: %d", rc);
            if (++failures >= 3) { failures = 0; address = address == 0x83 ? 0x80 : address+1; }
        }
    }
}
K_THREAD_DEFINE(drs_thread, 2048, run, NULL, NULL, NULL, 5, 0, 500);
