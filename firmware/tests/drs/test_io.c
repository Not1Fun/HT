#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include "../../src/platform/drs_io.c"

static int64_t clock_ms;
static bool tx_enabled,tx_failed,rx_failed,corrupt,exception,no_response,init_error,scan;
static uint8_t wire[8],rx[16],addresses[32];
static size_t used,left;
static unsigned int attempts,stop_after;
static jmp_buf finished;
int64_t k_uptime_get(void) { return clock_ms; }
void k_msleep(int ms) {clock_ms+=ms;}
int k_msgq_put(struct k_msgq *q,const void *p,int wait) {
    (void)wait;if(q->used==sizeof(q->data))return -ENOSPC;q->data[q->used++]=*(const uint8_t *)p;return 0;
}
int k_msgq_get(struct k_msgq *q,void *p,int wait) {
    if(!q->used) {clock_ms+=wait;return -EAGAIN;}*(uint8_t *)p=q->data[0];memmove(q->data,q->data+1,--q->used);return 0;
}
void k_msgq_purge(struct k_msgq *q) {q->used=0;}
k_spinlock_key_t k_spin_lock(struct k_spinlock *l) {assert(!l->held);l->held=1;return 0;}
void k_spin_unlock(struct k_spinlock *l,k_spinlock_key_t k) {(void)k;assert(l->held);l->held=0;}
bool device_is_ready(const struct device *d) {(void)d;return !init_error;}
int uart_irq_update(const struct device *d) {(void)d;return 1;}
int uart_err_check(const struct device *d) {(void)d;return rx_failed?1:0;}
int uart_irq_tx_ready(const struct device *d) {(void)d;return tx_enabled;}
int uart_irq_rx_ready(const struct device *d) {(void)d;return left!=0;}
int uart_fifo_fill(const struct device *d,const uint8_t *p,int n) {
    (void)d;if(tx_failed)return -EIO;if(n>3)n=3;memcpy(wire+used,p,(size_t)n);used+=(size_t)n;return n;
}
int uart_fifo_read(const struct device *d,uint8_t *p,int n) {
    (void)d;if((size_t)n>left)n=(int)left;memcpy(p,rx,(size_t)n);left-=(size_t)n;return n;
}
int uart_irq_tx_complete(const struct device *d) {(void)d;return used==8;}
void uart_irq_tx_disable(const struct device *d) {(void)d;tx_enabled=false;}
static void response_crc(uint8_t *f,size_t n) {
    unsigned int crc=65535;
    for(size_t i=0;i<n-2;++i) {crc^=f[i];for(int b=0;b<8;++b)crc=crc&1?(crc>>1)^40961:crc>>1;}
    f[n-2]=(uint8_t)crc;f[n-1]=(uint8_t)(crc>>8);
}
void uart_irq_tx_enable(const struct device *d) {
    if(stop_after && attempts>=stop_after)longjmp(finished,1);
    used=0;left=0;tx_enabled=true;
    for(int i=0;i<4 && tx_enabled;++i)interrupt(d,NULL);
    if(tx_failed)return;
    assert(used==8 && (wire[1]==3 || wire[1]==4));
    addresses[attempts++]=wire[0];
    if(no_response || (scan && wire[0]!=0x81))return;
    /* 寄存器响应前带一个噪声字节，验证接收重同步。 */
    rx[0]=0x55;rx[1]=wire[0];rx[2]=exception?(uint8_t)(wire[1]|0x80):wire[1];
    rx[3]=2;rx[4]=0;rx[5]=wire[3]==0xc3?32:7;
    left=exception?6:8;response_crc(rx+1,left-1);
    if(corrupt)rx[left-1]^=1;
    interrupt(d,NULL);
}
void uart_irq_rx_enable(const struct device *d) {(void)d;}
void uart_irq_err_enable(const struct device *d) {(void)d;}
int uart_irq_callback_user_data_set(const struct device *d,void (*f)(const struct device *,void *),void *p) {(void)d;(void)f;(void)p;return 0;}
int main(int argc,char **argv) {
    assert(argc==2);uint16_t value=123;int expected=0;
    if(!strcmp(argv[1],"init_error")) {init_error=true;run(NULL,NULL,NULL);assert(published.error==-ENODEV);return 0;}
    if(!strcmp(argv[1],"reconnect")) {
        scan=true;stop_after=10;
        if(!setjmp(finished))run(NULL,NULL,NULL);
        assert(addresses[0]==0x83 && addresses[2]==0x83 && addresses[3]==0x80 && addresses[6]==0x81);
        struct drs_snapshot s;drs_io_snapshot(&s);
        assert(s.valid && s.address==0x81 && s.sequence==1 && s.system==32);
        clock_ms+=2000;drs_io_snapshot(&s);assert(!s.valid);return 0;
    }
    if(!strcmp(argv[1],"corrupt")){corrupt=true;expected=-ETIMEDOUT;}
    if(!strcmp(argv[1],"exception")){exception=true;expected=-EPROTO;}
    if(!strcmp(argv[1],"timeout")){no_response=true;expected=-ETIMEDOUT;}
    if(!strcmp(argv[1],"tx_failure")){tx_failed=true;expected=-EIO;}
    if(!strcmp(argv[1],"rx_error")){rx_failed=true;expected=-EIO;}
    uint8_t stale=0x83;assert(k_msgq_put(&drs_rx_queue,&stale,0)==0);
    assert(read_register(0x83,DRS_FAULT,&value)==expected && !tx_enabled && clock_ms<=50);
    assert(value==(expected?123:7));
    return 0;
}
