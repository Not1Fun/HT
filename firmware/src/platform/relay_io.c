/** @brief Adapt the portable TCA9539 driver to Zephyr I2C on HT MAIN-01. */
#include "platform/relay_io.h"
#include "drivers/tca9539_safe.h"

#include <errno.h>
#include <zephyr/drivers/i2c.h>

static const struct i2c_dt_spec bus = I2C_DT_SPEC_GET(DT_NODELABEL(relay_io));

static int read_reg(void *ctx, uint8_t addr, uint8_t reg, uint8_t *value)
{
	const struct i2c_dt_spec *io = ctx;

	return i2c_reg_read_byte(io->bus, addr, reg, value);
}

static int write_reg(void *ctx, uint8_t addr, uint8_t reg, uint8_t value)
{
	const struct i2c_dt_spec *io = ctx;

	return i2c_reg_write_byte(io->bus, addr, reg, value);
}

static struct tca9539 relay = {
	.ctx = (void *)&bus,
	.addr = DT_REG_ADDR(DT_NODELABEL(relay_io)),
	.read_reg = read_reg,
	.write_reg = write_reg,
};

int relay_io_init(void)
{
	relay.ready = false;
	if (!i2c_is_ready_dt(&bus)) {
		return -ENODEV;
	}
	return tca9539_init(&relay);
}

int relay_io_check(void)
{
	return tca9539_verify_off(&relay);
}

int relay_io_read_panel(uint8_t *state)
{
	return tca9539_read_panel(&relay, state);
}

int relay_io_stop(void)
{
	int rc = relay_io_init();

	relay.ready = false;
	return rc;
}
