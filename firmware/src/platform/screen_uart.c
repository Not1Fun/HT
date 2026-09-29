/** @brief Buffer UART RX in interrupts and latch communication failures. */
#include "platform/screen_uart.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define RX_CAPACITY 2048U

struct uart_link {
	const struct device *device;
	struct k_spinlock lock;
	struct screen_uart_stats stats;
	uint8_t rx[RX_CAPACITY];
	size_t head;
	size_t tail;
	size_t count;
	int error;
	bool active;
};

static struct uart_link links[] = {
	[SCREEN_UART_DISPLAY] = { .device = DEVICE_DT_GET(DT_NODELABEL(usart3)) },
#if defined(CONFIG_HT_SCREEN_BRIDGE)
	[SCREEN_UART_HOST] = { .device = DEVICE_DT_GET(DT_NODELABEL(usart1)) },
#endif
};

#if defined(CONFIG_HT_SCREEN_BRIDGE)
BUILD_ASSERT(!IS_ENABLED(CONFIG_UART_CONSOLE), "Bridge owns USART1; disable UART console");
BUILD_ASSERT(!IS_ENABLED(CONFIG_LOG_BACKEND_UART), "Bridge owns USART1; disable UART logging");
#endif

static struct uart_link *get_link(enum screen_uart_port port)
{
	return (unsigned int)port < ARRAY_SIZE(links) ? &links[port] : NULL;
}

static int status_locked(const struct uart_link *link)
{
	return link->error != 0 ? link->error : link->active ? 0 : -EACCES;
}

static void fail_link(struct uart_link *link, int error, int flags)
{
	k_spinlock_key_t key = k_spin_lock(&link->lock);

	if (link->error == 0) {
		link->error = error;
	}
	if (flags > 0) {
		link->stats.uart_errors |= (uint32_t)flags;
	}
	link->active = false;
	k_spin_unlock(&link->lock, key);
	uart_irq_rx_disable(link->device);
	uart_irq_err_disable(link->device);
}

static void receive_irq(const struct device *device, void *context)
{
	struct uart_link *link = context;
	int rc = uart_irq_update(device);

	if (rc < 0) {
		fail_link(link, rc, 0);
		return;
	}
	rc = uart_err_check(device);
	if (rc != 0) {
		fail_link(link, rc < 0 ? rc : -EIO, rc);
		return;
	}
	rc = uart_irq_rx_ready(device);
	if (rc < 0) {
		fail_link(link, rc, 0);
		return;
	}
	if (rc == 0) {
		return;
	}
	uint8_t data[32];
	int count;

	do {
		count = uart_fifo_read(device, data, sizeof(data));
		if (count < 0) {
			fail_link(link, count, 0);
			return;
		}
		k_spinlock_key_t key = k_spin_lock(&link->lock);

		link->stats.rx_bytes += (uint32_t)count;
		if ((size_t)count > RX_CAPACITY - link->count) {
			link->stats.overflows++;
			link->error = -ENOBUFS;
			k_spin_unlock(&link->lock, key);
			fail_link(link, -ENOBUFS, 0);
			return;
		}
		for (int i = 0; i < count; ++i) {
			link->rx[link->head] = data[i];
			link->head = (link->head + 1U) % RX_CAPACITY;
		}
		link->count += (size_t)count;
		k_spin_unlock(&link->lock, key);
	} while (count == (int)sizeof(data));
	rc = uart_err_check(device);
	if (rc != 0) {
		fail_link(link, rc < 0 ? rc : -EIO, rc);
	}
}

int screen_uart_init(enum screen_uart_port port)
{
	struct uart_link *link = get_link(port);

	if (link == NULL) {
		return -EINVAL;
	}
	if (!device_is_ready(link->device)) {
		return -ENODEV;
	}
	screen_uart_stop(port);
	k_spinlock_key_t key = k_spin_lock(&link->lock);

	memset(&link->stats, 0, sizeof(link->stats));
	link->head = 0;
	link->tail = 0;
	link->count = 0;
	link->error = 0;
	k_spin_unlock(&link->lock, key);
	int rc = uart_irq_callback_user_data_set(link->device, receive_irq, link);

	if (rc != 0) {
		fail_link(link, rc, 0);
		return rc;
	}
	rc = uart_err_check(link->device);
	if (rc < 0) {
		fail_link(link, rc, 0);
		return screen_uart_check(port);
	}
	/* Pin setup/reset can leave a partial frame before this transport owns RX. */
	uint8_t stale;
	unsigned int discarded = 0;

	while (discarded < 32U) {
		rc = uart_poll_in(link->device, &stale);
		if (rc == -1) {
			break;
		}
		if (rc != 0) {
			fail_link(link, rc < 0 ? rc : -EIO, 0);
			return screen_uart_check(port);
		}
		discarded++;
	}
	if (discarded == 32U) {
		fail_link(link, -EBUSY, 0);
		return -EBUSY;
	}
	rc = uart_err_check(link->device);
	if (rc < 0) {
		fail_link(link, rc, 0);
		return rc;
	}
	key = k_spin_lock(&link->lock);
	link->active = true;
	k_spin_unlock(&link->lock, key);
	uart_irq_err_enable(link->device);
	uart_irq_rx_enable(link->device);
	rc = screen_uart_check(port);
	if (rc != 0) {
		screen_uart_stop(port);
	}
	return rc;
}

int screen_uart_read(enum screen_uart_port port, uint8_t *data, size_t capacity)
{
	struct uart_link *link = get_link(port);

	if (link == NULL || data == NULL || capacity == 0) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&link->lock);
	int rc = status_locked(link);

	if (rc == 0) {
		size_t count = MIN(capacity, link->count);

		for (size_t i = 0; i < count; ++i) {
			data[i] = link->rx[link->tail];
			link->tail = (link->tail + 1U) % RX_CAPACITY;
		}
		link->count -= count;
		rc = (int)count;
	}
	k_spin_unlock(&link->lock, key);
	return rc;
}

int screen_uart_write(enum screen_uart_port port, const uint8_t *data, size_t length)
{
	struct uart_link *link = get_link(port);

	if (link == NULL || data == NULL) {
		return -EINVAL;
	}
	for (size_t i = 0; i < length; ++i) {
		int rc = screen_uart_check(port);

		if (rc != 0) {
			return rc;
		}
		uart_poll_out(link->device, data[i]);
		k_spinlock_key_t key = k_spin_lock(&link->lock);

		link->stats.tx_bytes++;
		k_spin_unlock(&link->lock, key);
	}
	return screen_uart_check(port);
}

int screen_uart_check(enum screen_uart_port port)
{
	struct uart_link *link = get_link(port);

	if (link == NULL) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&link->lock);
	int rc = status_locked(link);

	k_spin_unlock(&link->lock, key);
	return rc;
}

int screen_uart_get_stats(enum screen_uart_port port, struct screen_uart_stats *stats)
{
	struct uart_link *link = get_link(port);

	if (link == NULL || stats == NULL) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&link->lock);

	*stats = link->stats;
	int rc = status_locked(link);

	k_spin_unlock(&link->lock, key);
	return rc;
}

void screen_uart_stop(enum screen_uart_port port)
{
	struct uart_link *link = get_link(port);

	if (link == NULL || !device_is_ready(link->device)) {
		return;
	}
	uart_irq_rx_disable(link->device);
	uart_irq_err_disable(link->device);
	k_spinlock_key_t key = k_spin_lock(&link->lock);

	link->active = false;
	k_spin_unlock(&link->lock, key);
}
