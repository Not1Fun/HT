/* @brief 安全钩子主机测试的串口恢复入口。 */
#ifndef TEST_BOOT_SERIAL_H
#define TEST_BOOT_SERIAL_H
struct boot_uart_funcs {
	int (*read)(char *, int, int *);
	void (*write)(const char *, int);
};
void boot_serial_start(const struct boot_uart_funcs *functions);
#endif
