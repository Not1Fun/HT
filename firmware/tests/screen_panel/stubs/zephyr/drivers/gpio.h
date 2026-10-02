#pragma once
#include <stdbool.h>
#include <stdint.h>
struct device { int unused; };
extern const struct device mock_gpio;
typedef uint32_t gpio_port_value_t;
typedef uint32_t gpio_port_pins_t;
struct gpio_dt_spec { const struct device *port; uint8_t pin; uint32_t flags; };
struct gpio_callback { int unused; };
#define GPIO_SPEC_encoder_a_gpios {&mock_gpio,6,0}
#define GPIO_SPEC_encoder_b_gpios {&mock_gpio,7,0}
#define GPIO_DT_SPEC_GET(n,p) GPIO_SPEC_##p
#define GPIO_INPUT 1
#define GPIO_INT_DISABLE 0
#define GPIO_INT_EDGE_BOTH 3
bool gpio_is_ready_dt(const struct gpio_dt_spec *pin);
int gpio_port_get_raw(const struct device *port,gpio_port_value_t *value);
int gpio_pin_configure_dt(const struct gpio_dt_spec *pin,int flags);
int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *pin,int flags);
void gpio_init_callback(struct gpio_callback *cb,void (*fn)(const struct device *,struct gpio_callback *,gpio_port_pins_t),uint32_t mask);
int gpio_add_callback(const struct device *port,struct gpio_callback *cb);
int gpio_remove_callback(const struct device *port,struct gpio_callback *cb);
