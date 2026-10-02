/* @brief 安全钩子主机测试的时钟和永停替身。 */
#ifndef TEST_KERNEL_H
#define TEST_KERNEL_H
#include <stdbool.h>
#include <stdint.h>
#define K_FOREVER (-1)
int64_t k_uptime_get(void);
void k_sleep(int timeout);
#endif
