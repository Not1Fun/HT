/* @brief 安全钩子主机测试的 MCUboot 状态接口。 */
#ifndef TEST_MCUBOOT_STATUS_H
#define TEST_MCUBOOT_STATUS_H
typedef enum {
	MCUBOOT_STATUS_STARTUP = 0,
	MCUBOOT_STATUS_UPGRADING,
	MCUBOOT_STATUS_BOOTABLE_IMAGE_FOUND
} mcuboot_status_type_t;
void mcuboot_status_change(mcuboot_status_type_t status);
#endif
