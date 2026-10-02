/* @brief 引导期间保持数字输出安全与继电器全断，故障只允许串口恢复。 */
#include "platform/board_io.h"
#include "platform/relay_io.h"

#include <bootutil/boot_hooks.h>
#include <bootutil/mcuboot_status.h>
#include <boot_serial/boot_serial.h>
#include <zephyr/kernel.h>

int boot_console_init(void);
int console_read(char *text, int capacity, int *newline);
void console_write(const char *text, int count);

static const struct boot_uart_funcs recovery_uart = {
	.read = console_read,
	.write = console_write,
};
static bool ready;
static int64_t next_check;

static void stop(void)
{
	ready = false;
	(void)relay_io_stop();
}

/* MCUboot恢复循环调用此扩展点；跳转前另作全断检查，不喂外部看门狗。 */
void mcuboot_watchdog_feed(void)
{
	int64_t now = k_uptime_get();

	if (ready && now >= next_check) {
		next_check = now + 200;
		if (relay_io_check() != 0) {
			stop();
		}
	}
}

fih_ret boot_go_hook(struct boot_rsp *response)
{
	(void)response;
	if (!ready) {
		FIH_RET(FIH_FAILURE);
	}
	FIH_RET(FIH_BOOT_HOOK_REGULAR);
}

void mcuboot_status_change(mcuboot_status_type_t status)
{
	if (status == MCUBOOT_STATUS_STARTUP) {
		ready = board_io_init_digital() == 0;
		if (ready) {
			ready = relay_io_init() == 0;
		}
		if (!ready) {
			stop();
		}
		next_check = k_uptime_get() + 200;
	} else if (status == MCUBOOT_STATUS_BOOTABLE_IMAGE_FOUND) {
		if (ready && relay_io_check() == 0) {
			return;
		}
		stop();
		if (boot_console_init() == 0) {
			boot_serial_start(&recovery_uart);
		}
		/* 串口本身不可用时同样禁止进入应用。 */
		for (;;) {
			k_sleep(K_FOREVER);
		}
	}
}
