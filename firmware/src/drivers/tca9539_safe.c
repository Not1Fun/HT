/* @brief 校验继电器寄存器和单抽头互斥，失败即撤销就绪状态。 */
#include "tca9539_safe.h"

#include <stddef.h>

enum {
    INPUT1 = 0x01,
    OUTPUT0 = 0x02,
    POLARITY1 = 0x05,
    CONFIG0 = 0x06,
    CONFIG1 = 0x07
};

static bool valid_bus(const struct tca9539 *dev)
{
    return dev->read_reg != NULL && dev->write_reg != NULL &&
           dev->addr >= 0x74 && dev->addr <= 0x77;
}

static int fail(struct tca9539 *dev, int error)
{
    dev->ready = false;
    return error;
}

static int read_expected(struct tca9539 *dev, uint8_t reg, uint8_t expected)
{
    uint8_t value = 0;

    if (dev->read_reg(dev->ctx, dev->addr, reg, &value) != 0) {
        return fail(dev, TCA9539_ERR_IO);
    }
    if (value != expected) {
        return fail(dev, TCA9539_ERR_VERIFY);
    }
    return TCA9539_OK;
}

static int check_ready(struct tca9539 *dev)
{
    if (dev == NULL) {
        return TCA9539_ERR_ARG;
    }
    if (!valid_bus(dev)) {
        return fail(dev, TCA9539_ERR_ARG);
    }
    if (!dev->ready) {
        return TCA9539_ERR_NOT_READY;
    }
    return TCA9539_OK;
}

int tca9539_init(struct tca9539 *dev)
{
    static const struct {
        uint8_t reg;
        uint8_t value;
    } sequence[] = {
        {OUTPUT0, 0x00},
        {CONFIG1, 0xff},
        {POLARITY1, 0x00},
        {CONFIG0, 0x00}
    };

    if (dev == NULL) {
        return TCA9539_ERR_ARG;
    }
    dev->ready = false;
    if (!valid_bus(dev)) {
        return TCA9539_ERR_ARG;
    }

    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        int result;

        if (dev->write_reg(dev->ctx, dev->addr, sequence[i].reg,
                           sequence[i].value) != 0) {
            return fail(dev, TCA9539_ERR_IO);
        }
        result = read_expected(dev, sequence[i].reg, sequence[i].value);
        if (result != TCA9539_OK) {
            return result;
        }
    }
    dev->output = 0;
    dev->ready = true;
    return TCA9539_OK;
}

int tca9539_verify_off(struct tca9539 *dev)
{
    int result = check_ready(dev);

    if (result != TCA9539_OK) {
        return result;
    }
    result = read_expected(dev, OUTPUT0, 0x00);
    if (result != TCA9539_OK) {
        return result;
    }
    return read_expected(dev, CONFIG0, 0x00);
}

int tca9539_read_panel(struct tca9539 *dev, uint8_t *value)
{
    uint8_t input = 0;
    int result;

    if (dev == NULL) {
        return TCA9539_ERR_ARG;
    }
    if (value == NULL) {
        return fail(dev, TCA9539_ERR_ARG);
    }
    result = check_ready(dev);
    if (result != TCA9539_OK) {
        return result;
    }
    if (dev->read_reg(dev->ctx, dev->addr, INPUT1, &input) != 0) {
        return fail(dev, TCA9539_ERR_IO);
    }
    *value = input;
    return TCA9539_OK;
}

int tca9539_verify_output(struct tca9539 *dev)
{
    int rc = check_ready(dev);
    if (rc == 0) rc = read_expected(dev, OUTPUT0, dev->output);
    if (rc == 0) rc = read_expected(dev, CONFIG0, 0);
    if (rc == 0) rc = read_expected(dev, CONFIG1, 0xff);
    if (rc == 0) rc = read_expected(dev, POLARITY1, 0);
    return rc;
}

int tca9539_set_output(struct tca9539 *dev, uint8_t output)
{
    int rc = check_ready(dev);
    if (rc != 0) return rc;
    uint8_t taps = output & 0xfeu;
    uint8_t previous = dev->output & 0xfeu;
    if ((taps & (taps - 1u)) != 0 ||
        (previous && taps && previous != taps) ||
        (output != 0 && (previous || taps) && ((output ^ dev->output) & 1u))) {
        return fail(dev, TCA9539_ERR_ARG);
    }
    rc = tca9539_verify_output(dev);
    if (rc != 0) return rc;
    if (dev->write_reg(dev->ctx, dev->addr, OUTPUT0, output) != 0)
        return fail(dev, TCA9539_ERR_IO);
    rc = read_expected(dev, OUTPUT0, output);
    if (rc == 0) dev->output = output;
    return rc;
}
