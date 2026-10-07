#include "core/bench.h"
#include <assert.h>
#include <stdio.h>

static struct bench b;
static int64_t clock_ms, changed;
static uint8_t port;
static uint16_t amplitude;
static bool signal_on, cancel, fail, cancel_select;
static int muted(void *ctx) { (void)ctx; amplitude=0; signal_on=false; return 0; }
static int select_port(void *ctx, uint8_t value)
{
    (void)ctx;
    if (fail) return -1;
    uint8_t taps=value & 0xfeu;
    assert(!(taps & (taps-1u)));
    assert(amplitude==0 && !signal_on);
    if (value) {
        assert(!(port & 0xfeu) || !taps || taps==(port & 0xfeu));
        assert(!((port^value)&1u) || !((port|value)&0xfeu));
        if (port != value) assert(clock_ms-changed>=30);
    }
    if (port!=value) changed=clock_ms;
    port=value;
    if (value && cancel_select) cancel=true;
    return 0;
}
static int start(void *ctx, uint32_t hz) { (void)ctx; assert(hz==2000); signal_on=true; return 0; }
static int level(void *ctx, uint16_t a) { (void)ctx; assert(signal_on && !cancel); amplitude=a; return 0; }
static int64_t now(void *ctx) { (void)ctx; return clock_ms; }
static bool cancelled(void *ctx) { (void)ctx; return cancel; }
static struct power_reading reading;
static void step(int ms)
{
    while(ms--) { ++clock_ms; reading.time_ms=clock_ms; bench_poll(&b,true,&reading,clock_ms); }
}
static void setup(void)
{
    clock_ms=changed=0; port=amplitude=0; signal_on=cancel=fail=cancel_select=false;
    reading=(struct power_reading){.valid=true,.voltage_mv=220,.current_ma=6,.apparent_mva=1};
    struct power_ops ops={.mute=muted,.select=select_port,.start=start,.level=level,.now=now,.cancelled=cancelled};
    assert(bench_init(&b,&ops)==0);
}
int main(void)
{
    setup(); assert(bench_start(&b,9,2000,25,false,0)<0);
    assert(bench_start(&b,2,2000,101,true,0)<0);
    assert(bench_start(&b,2,3000,25,true,0)<0);
    for(uint8_t relay=0;relay<=8;++relay) {
        assert(bench_start(&b,relay,2000,25,false,clock_ms)==0); step(200);
        assert(b.state==BENCH_ON && port==bench_mask(relay) && !signal_on);
    }
    step(30000); assert(b.state==BENCH_IDLE && port==0);
    /* 有波形时换高/低变压器：必须先静音、断抽头，再切K1。 */
    assert(bench_start(&b,8,2000,100,true,clock_ms)==0); step(400);
    assert(b.state==BENCH_ON && amplitude==70 && port==0x81);
    assert(bench_start(&b,2,2000,10,true,clock_ms)==0);
    assert(amplitude==0 && !signal_on); step(400);
    assert(amplitude==7 && port==2); step(10000);
    assert(b.state==BENCH_IDLE && amplitude==0 && port==0);
    for(int phase=0;phase<450;phase+=15) {
        setup(); assert(bench_start(&b,8,2000,25,true,0)==0); step(phase);
        cancel=true; step(100); assert(b.state==BENCH_IDLE && !signal_on && port==0);
    }
    setup(); cancel_select=true; assert(bench_start(&b,8,2000,25,true,0)==0); step(200);
    assert(b.state==BENCH_IDLE && port==0 && amplitude==0);
    setup(); assert(bench_start(&b,2,2000,25,true,0)==0); step(400);
    reading.apparent_mva=1001; step(1); assert(b.state==BENCH_FAULT && b.error==POWER_ERROR_LIMIT && port==0);
    setup(); assert(bench_start(&b,2,2000,25,true,0)==0); step(400);
    reading.valid=false; step(1); assert(b.state==BENCH_FAULT && b.error==POWER_ERROR_SAMPLE && !signal_on);
    setup(); assert(bench_start(&b,2,2000,25,true,0)==0); step(400);
    bench_poll(&b,false,&reading,clock_ms); assert(amplitude==0); step(100); assert(port==0);
    setup(); assert(bench_start(&b,2,2000,25,false,0)==0); fail=true; step(50);
    assert(b.state==BENCH_FAULT && !b.fault_stopped);
    puts("PASS bench relay ordering, limits, timeout, cancellation and feedback trips");
    return 0;
}
