/* @brief 故障注入验证真实引导安全钩子，不连接 GPIO、I2C 或串口。 */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <bootutil/boot_hooks.h>
#include <bootutil/mcuboot_status.h>
#include <boot_serial/boot_serial.h>

fih_ret boot_go_hook(struct boot_rsp *response);
void mcuboot_watchdog_feed(void);
static int board_error, relay_error, check_error, console_error;
static int board_calls, relay_calls, check_calls, stop_calls, serial_calls;
static int console_calls;
static int64_t now;
static jmp_buf recovery;

int board_io_init_digital(void) { board_calls++; return board_error; }
int relay_io_init(void) { relay_calls++; return relay_error; }
int relay_io_check(void) { check_calls++; return check_error; }
int relay_io_stop(void) { stop_calls++; return -1; }
int64_t k_uptime_get(void) { return now; }
int boot_console_init(void) { console_calls++; return console_error; }
int console_read(char *text, int capacity, int *newline)
{
	(void)text; (void)capacity; (void)newline; return 0;
}
void console_write(const char *text, int count) { (void)text; (void)count; }
void boot_serial_start(const struct boot_uart_funcs *functions)
{
	assert(functions->read == console_read && functions->write == console_write);
	serial_calls++;
	longjmp(recovery, 1);
}
void k_sleep(int timeout)
{
	assert(timeout == -1);
	longjmp(recovery, 2);
}

static void expect_recovery(int expected)
{
	int result = setjmp(recovery);
	if (result == 0) {
		mcuboot_status_change(MCUBOOT_STATUS_BOOTABLE_IMAGE_FOUND);
		assert(!"unsafe return from final check");
	}
	assert(result == expected);
	assert(boot_go_hook(NULL) == FIH_FAILURE);
	assert(console_calls == 1);
	assert(serial_calls == (expected == 1));
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (strcmp(argv[1], "board_failure") == 0) {
		board_error = -1;
		mcuboot_status_change(MCUBOOT_STATUS_STARTUP);
		assert(board_calls == 1 && relay_calls == 0 && stop_calls == 1);
		assert(boot_go_hook(NULL) == FIH_FAILURE);
	} else if (strcmp(argv[1], "relay_failure") == 0) {
		relay_error = -1;
		mcuboot_status_change(MCUBOOT_STATUS_STARTUP);
		assert(relay_calls == 1 && stop_calls == 1);
		assert(boot_go_hook(NULL) == FIH_FAILURE);
	} else if (strcmp(argv[1], "unready_final") == 0) {
		assert(boot_go_hook(NULL) == FIH_FAILURE);
		expect_recovery(1);
		assert(check_calls == 0 && stop_calls == 1);
	} else {
		mcuboot_status_change(MCUBOOT_STATUS_STARTUP);
		assert(board_calls == 1 && relay_calls == 1 && stop_calls == 0);
		assert(boot_go_hook(NULL) == FIH_BOOT_HOOK_REGULAR);
		if (strcmp(argv[1], "startup") == 0) {
			now = 199; mcuboot_watchdog_feed(); assert(check_calls == 0);
			now = 200; mcuboot_watchdog_feed(); assert(check_calls == 1);
			now = 399; mcuboot_watchdog_feed(); assert(check_calls == 1);
			now = 400; mcuboot_watchdog_feed(); assert(check_calls == 2);
			mcuboot_status_change(MCUBOOT_STATUS_BOOTABLE_IMAGE_FOUND);
			assert(check_calls == 3 && stop_calls == 0 && serial_calls == 0);
		} else if (strcmp(argv[1], "periodic_failure") == 0) {
			check_error = -1; now = 200; mcuboot_watchdog_feed();
			assert(check_calls == 1 && stop_calls == 1);
			assert(boot_go_hook(NULL) == FIH_FAILURE);
			check_error = 0; now = 1000; mcuboot_watchdog_feed();
			assert(check_calls == 1);
			expect_recovery(1);
			assert(check_calls == 1 && stop_calls == 2);
		} else if (strcmp(argv[1], "final_failure") == 0) {
			check_error = -1;
			expect_recovery(1);
			assert(check_calls == 1 && stop_calls == 1);
		} else if (strcmp(argv[1], "serial_failure") == 0) {
			check_error = -1; console_error = -1;
			expect_recovery(2);
		} else {
			assert(!"unknown test");
		}
	}
	puts("boot safe test passed");
	return 0;
}
