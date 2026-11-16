#ifndef DRS_TEST_MOCK_H
#define DRS_TEST_MOCK_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#define ARG_UNUSED(x) (void)(x)
#define LOG_MODULE_REGISTER(x) typedef int log_declaration
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define DT_NODELABEL(x) 0
#define DEVICE_DT_GET(x) (&device)
#define K_MSGQ_DEFINE(name,size,count,align) struct k_msgq name
#define K_THREAD_DEFINE(...) typedef int thread_declaration
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
struct device { int unused; };
static const struct device device;
struct k_spinlock { int held; };
typedef int k_spinlock_key_t;
typedef int atomic_t;
struct k_msgq { uint8_t data[32];size_t used; };
static inline void atomic_set(atomic_t *a,int v) { *a=v; }
static inline int atomic_get(atomic_t *a) { return *a; }
static inline void atomic_clear(atomic_t *a) { *a=0; }
k_spinlock_key_t k_spin_lock(struct k_spinlock *);
void k_spin_unlock(struct k_spinlock *,k_spinlock_key_t);
int64_t k_uptime_get(void);
void k_msleep(int);
int k_msgq_get(struct k_msgq *,void *,int);
int k_msgq_put(struct k_msgq *,const void *,int);
void k_msgq_purge(struct k_msgq *);
bool device_is_ready(const struct device *);
int uart_irq_update(const struct device *);
int uart_err_check(const struct device *);
int uart_irq_tx_ready(const struct device *);
int uart_irq_rx_ready(const struct device *);
int uart_fifo_fill(const struct device *,const uint8_t *,int);
int uart_fifo_read(const struct device *,uint8_t *,int);
int uart_irq_tx_complete(const struct device *);
void uart_irq_tx_disable(const struct device *);
void uart_irq_tx_enable(const struct device *);
void uart_irq_rx_enable(const struct device *);
void uart_irq_err_enable(const struct device *);
int uart_irq_callback_user_data_set(const struct device *,void (*)(const struct device *,void *),void *);
#endif
