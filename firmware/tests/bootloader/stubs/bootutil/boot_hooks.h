/* @brief 安全钩子主机测试的 MCUboot 返回值替身。 */
#ifndef TEST_BOOT_HOOKS_H
#define TEST_BOOT_HOOKS_H
typedef int fih_ret;
struct boot_rsp { int unused; };
#define FIH_FAILURE (-1)
#define FIH_BOOT_HOOK_REGULAR 1
#define FIH_RET(value) return (value)
#endif
