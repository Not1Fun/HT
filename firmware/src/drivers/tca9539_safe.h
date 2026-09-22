/* @brief TCA9539 安全初始化、全断寄存器检查与面板输入接口。 */
#ifndef TCA9539_SAFE_H
#define TCA9539_SAFE_H

#include <stdbool.h>
#include <stdint.h>

enum tca9539_result {
    TCA9539_OK = 0,
    TCA9539_ERR_ARG = -1,
    TCA9539_ERR_NOT_READY = -2,
    TCA9539_ERR_IO = -3,
    TCA9539_ERR_VERIFY = -4
};

/* 对象须零初始化，所有调用由同一事务服务串行执行。 */
struct tca9539 {
    void *ctx;
    uint8_t addr; /* 7 位地址：0x74..0x77。 */
    /* 回调返回 0 表示成功，其他值统一转换为 ERR_IO。 */
    int (*read_reg)(void *ctx, uint8_t addr, uint8_t reg, uint8_t *value);
    int (*write_reg)(void *ctx, uint8_t addr, uint8_t reg, uint8_t value);
    bool ready; /* 仅驱动修改；任何失败后必须重新 init。 */
};

int tca9539_init(struct tca9539 *dev);
/* 检查输出锁存器和方向寄存器，不代表继电器触点已断开。 */
int tca9539_verify_off(struct tca9539 *dev);
/* 返回 Port1 原始电平；失败不修改调用者的 value。 */
int tca9539_read_panel(struct tca9539 *dev, uint8_t *value);

#endif
