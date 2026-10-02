#ifndef HT_OUTPUT_MOCK_H
#define HT_OUTPUT_MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#define ARG_UNUSED(x) (void)(x)
#define BIT(n) (1u << (n))
#define DT_PATH(x) 0
#define DT_NODELABEL(x) 0
#define DEVICE_DT_GET(x) (&device)
#define GPIO_DT_SPEC_GET(node, pin) {&device, 0}
#define GPIO_INT_EDGE_TO_ACTIVE 1
#define GPIO_OUTPUT_INACTIVE 0
#define WDT_FLAG_RESET_SOC 1
#define WDT_OPT_PAUSE_HALTED_BY_DBG 1
#define K_FOREVER (-1)
#define K_SEM_DEFINE(name, initial, limit) int name
#define K_THREAD_DEFINE(name, stack, fn, a, b, c, priority, options, delay) \
    typedef int name##_declaration
struct device { int unused; };
static const struct device device;
struct gpio_dt_spec { const struct device *port; unsigned int pin; };
struct gpio_callback { int unused; };
typedef uint32_t gpio_port_pins_t;
struct wdt_timeout_cfg { struct { unsigned int min, max; } window; unsigned int flags; };
struct k_spinlock { bool locked; };
typedef int k_spinlock_key_t;
static int64_t clock_time;
static int feeds, steps;
static jmp_buf done;
static void (*between_steps)(void);
static inline int64_t k_uptime_get(void) { return clock_time; }
static inline int k_spin_lock(struct k_spinlock *lock) { assert(!lock->locked);lock->locked=true;return 0; }
static inline void k_spin_unlock(struct k_spinlock *lock, int key) { (void)key;lock->locked=false; }
static inline void k_sem_give(int *sem) { *sem=1; }
static inline void k_sem_take(int *sem, int timeout) { (void)timeout;assert(*sem); }
static inline void k_msleep(int ms) {
    clock_time+=ms;
    if(between_steps) between_steps();
    if(--steps<=0) longjmp(done,1);
}
static inline bool device_is_ready(const struct device *dev) { (void)dev;return true; }
static inline void gpio_init_callback(struct gpio_callback *cb,
    void (*handler)(const struct device *, struct gpio_callback *, gpio_port_pins_t), uint32_t mask)
    {(void)cb;(void)handler;(void)mask;}
static inline int gpio_add_callback(const struct device *d,struct gpio_callback *cb) {(void)d;(void)cb;return 0;}
static inline int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *p,int flags) {(void)p;(void)flags;return 0;}
static inline int gpio_pin_configure_dt(const struct gpio_dt_spec *p,int flags) {(void)p;(void)flags;return 0;}
static inline int gpio_pin_set_dt(const struct gpio_dt_spec *p,bool value) {(void)p;(void)value;return 0;}
static inline int wdt_install_timeout(const struct device *d,const struct wdt_timeout_cfg *cfg)
    {(void)d;assert(cfg->window.max==1000);return 0;}
static inline int wdt_setup(const struct device *d,int flags) {(void)d;(void)flags;return 0;}
static inline int wdt_feed(const struct device *d,int channel) {(void)d;(void)channel;feeds++;return 0;}
#endif
