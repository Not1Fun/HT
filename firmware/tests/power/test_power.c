#include "core/power.h"
#include "core/rms.h"
#include "drivers/tca9539_safe.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { int64_t now; uint8_t ports[64]; unsigned int count; int fail; int starts, mutes; uint16_t amp; } hw;
static int mute(void *ctx) { (void)ctx; hw.mutes++; hw.amp=0; return hw.fail==1?-1:0; }
static int select_port(void *ctx, uint8_t output)
{
    (void)ctx;
    hw.now += 5;
    if(hw.fail==2) return -1;
    assert(hw.count<64); hw.ports[hw.count++]=output; return 0;
}
static int start_signal(void *ctx, uint32_t frequency) { (void)ctx; assert(frequency==2000); hw.starts++; return hw.fail==3?-1:0; }
static int set_level(void *ctx, uint16_t amp) { (void)ctx; hw.amp=amp; return hw.fail==4?-1:0; }
static int64_t clock_ms(void *ctx) { (void)ctx; return hw.now; }
static struct power initialized(void)
{
    memset(&hw,0,sizeof(hw));
    struct power p;
    const struct power_ops ops={NULL,mute,select_port,start_signal,set_level,clock_ms,NULL};
    assert(power_init(&p,&ops)==0);
    return p;
}
static void tick(struct power *p, int64_t dt, bool okay, struct power_reading *r)
{
    hw.now+=dt;
    if(r) {r->time_ms=hw.now;r->sequence++;}
    power_poll(p,okay,r,hw.now);
}
static struct power running(uint8_t range)
{
    struct power p=initialized();
    assert(power_start(&p,range,2000,10000,hw.now)==0);
    for(int i=0;i<4;i++) tick(&p,30,true,NULL);
    assert(p.state==POWER_ZERO && hw.amp==0 && hw.starts==1);
    struct power_reading r={.valid=true};
    tick(&p,50,true,&r);
    assert(p.state==POWER_RUNNING && p.amplitude==0);
    return p;
}
static void sequence(void)
{
    struct power p=running(4);
    assert(hw.count==3 && hw.ports[0]==0 && hw.ports[1]==1 && hw.ports[2]==33);
    power_stop(&p,hw.now); assert(p.state==POWER_STOPPING && hw.amp==0);
    tick(&p,30,true,NULL);assert(p.state==POWER_RELEASE && hw.ports[3]==1);
    tick(&p,30,true,NULL);assert(p.state==POWER_IDLE && hw.ports[4]==0);
}
static void timing(void)
{
    struct power p=initialized();
    assert(power_start(&p,0,2000,1000,0)==0);
    tick(&p,29,true,NULL);assert(hw.count==0);
    tick(&p,1,true,NULL);assert(hw.count==1 && p.deadline==65);
    tick(&p,29,true,NULL);assert(hw.count==1);
    tick(&p,1,true,NULL);assert(hw.count==2);
}
static void all_ranges(void)
{
    for(uint8_t i=0;i<7;i++) {
        struct power p=running(i);
        assert(p.output==(uint8_t)((i>=4?1:0)|(1u<<(i+1))));
    }
    struct power p=initialized();
    assert(power_start(&p,7,2000,1000,0)<0);
    assert(power_start(&p,0,2000,0,0)<0);
    assert(power_start(&p,0,2000,50001,0)<0);
    assert(power_start(&p,0,3000,1000,0)<0 && hw.starts==0);
}
static void stop_stages(void)
{
    for(int phase=0;phase<5;phase++) {
        struct power p=initialized();assert(power_start(&p,6,2000,1000,0)==0);
        for(int n=0;n<phase;n++) tick(&p,30,true,NULL);
        power_stop(&p,hw.now);int starts=hw.starts;
        int64_t end=p.deadline;power_stop(&p,hw.now+10);assert(p.deadline==end);
        tick(&p,30,false,NULL);tick(&p,30,false,NULL);
        assert(p.state==POWER_IDLE && hw.starts==starts && hw.amp==0);
    }
}
static void interlock_stages(void)
{
    for(int phase=0;phase<5;phase++) {
        struct power p=initialized();assert(power_start(&p,4,2000,1000,0)==0);
        for(int n=0;n<phase;n++) tick(&p,30,true,NULL);
        int starts=hw.starts;tick(&p,1,false,NULL);tick(&p,500,true,NULL);
        assert(p.state==POWER_FAULT && p.error==POWER_ERROR_INTERLOCK && hw.amp==0 && hw.starts==starts);
    }
}
static void io_failure(void)
{
    for(int f=1;f<=4;f++) {
        struct power p=initialized();
        if(f==1) hw.fail=f;
        int rc=power_start(&p,3,2000,10000,0);
        if(f==1) {assert(rc<0 && p.state==POWER_FAULT);continue;}
        hw.fail=f;
        for(int n=0;n<4;n++) tick(&p,30,true,NULL);
        struct power_reading r={.valid=true};
        tick(&p,50,true,&r);tick(&p,50,true,&r);
        assert(p.state==POWER_FAULT && p.error==POWER_ERROR_IO && hw.amp==0);
        if(f==2) assert(!p.fault_stopped);
    }
}
static void stale(void)
{
    struct power p=running(3);
    struct power_reading r={.valid=true,.time_ms=hw.now-151,.sequence=1};
    power_poll(&p,true,&r,hw.now);assert(p.state==POWER_FAULT && p.error==POWER_ERROR_SAMPLE);
    p=initialized();assert(power_start(&p,3,2000,1000,0)==0);
    for(int n=0;n<4;n++) tick(&p,30,true,NULL);
    tick(&p,249,true,NULL);assert(p.state==POWER_ZERO);
    tick(&p,1,true,NULL);assert(p.state==POWER_FAULT);
}
static void regulation(void)
{
    struct power p=running(3);
    struct power_reading r={.valid=true};
    for(int n=0;n<200;n++) {
        float va=50.0f*hw.amp*hw.amp/1000000.0f;
        r.apparent_mva=(uint32_t)(va*1000);r.voltage_mv=(uint32_t)(sqrtf(va*30)*1000);
        r.current_ma=(uint32_t)(sqrtf(va/30)*1000);
        tick(&p,50,true,&r);
        assert(p.state==POWER_RUNNING && p.amplitude<=(hw.now-p.started)*2047/1000);
    }
    assert(r.apparent_mva>9500 && r.apparent_mva<10500);
    r.apparent_mva=20000;uint16_t before=p.amplitude;tick(&p,50,true,&r);assert(p.amplitude<before);
}
static void limits(void)
{
    for(int ch=0;ch<3;ch++) {
        struct power p=running(3);struct power_reading r={.valid=true};
        if(ch==0) r.voltage_mv=40667;
        else if(ch==1) r.current_ma=1355;
        else r.apparent_mva=52501;
        tick(&p,50,true,&r);assert(p.error==POWER_ERROR_LIMIT && hw.amp==0);
    }
}
static void open_short(void)
{
    struct power p=running(0);p.level_milli=2047000;p.started=-2000;p.low_since=hw.now-1100;
    struct power_reading r={.valid=true,.sequence=p.sequence};tick(&p,50,true,&r);assert(p.error==POWER_ERROR_OPEN);
    p=running(0);p.started=-1000;r=(struct power_reading){.valid=true,.sequence=p.sequence,.current_ma=1000,.voltage_mv=99};
    tick(&p,50,true,&r);assert(p.error==POWER_ERROR_SHORT);
}
static void continuous(void)
{
    struct power p=running(3);struct power_reading r={.valid=true,.current_ma=577,.voltage_mv=17320,.apparent_mva=10000};
    for(int n=0;n<1300;n++) tick(&p,50,true,&r);
    assert(p.state==POWER_RUNNING && hw.now-p.started>60000);
}
static uint32_t codes[8500];
static void rms_scaling(void)
{
    for(int n=0;n<8500;n++) {
        float s=sinf(6.283185307179586f*(n%85)/85.0f);
        unsigned int v=(unsigned int)lroundf(2048+100*s),i=(unsigned int)lroundf(2048+50*s);
        codes[n]=v|(i<<16);
    }
    struct rms a;rms_reset(&a);
    for(int n=0;n<10;n++) assert(rms_add(&a,codes+n*850,850)==0);
    struct power_reading r={0};assert(rms_result(&a,&r)==0);
    assert(r.voltage_mv>22000 && r.voltage_mv<22500 && r.current_ma>302 && r.current_ma<308);
    assert(r.apparent_mva==(uint64_t)r.voltage_mv*r.current_ma/1000);
    assert(rms_add(&a,codes,1)<0);
}
static void rms_clipping(void)
{
    struct rms a;rms_reset(&a);codes[0]=2048u|(4095u<<16);
    assert(rms_add(&a,codes,1)<0);
    for(int i=0;i<8500;i++) codes[i]=1000u|(2048u<<16);
    rms_reset(&a);assert(rms_add(&a,codes,8500)==0);
    struct power_reading r={0};assert(rms_result(&a,&r)<0 && !r.valid);
}
static uint8_t regs[8];
static int read_reg(void *ctx,uint8_t addr,uint8_t reg,uint8_t *v) {(void)ctx;(void)addr;*v=regs[reg];return 0;}
static int write_reg(void *ctx,uint8_t addr,uint8_t reg,uint8_t v) {(void)ctx;(void)addr;regs[reg]=v;return 0;}
static void relay_mutex(void)
{
    struct tca9539 dev={.addr=0x74,.read_reg=read_reg,.write_reg=write_reg};
    assert(tca9539_init(&dev)==0);assert(tca9539_set_output(&dev,2)==0);
    assert(tca9539_set_output(&dev,4)<0 && regs[2]==2);
    assert(tca9539_init(&dev)==0);assert(tca9539_set_output(&dev,6)<0 && regs[2]==0);
    assert(tca9539_init(&dev)==0);assert(tca9539_set_output(&dev,1)==0);
    assert(tca9539_set_output(&dev,33)==0);assert(tca9539_set_output(&dev,2)<0 && regs[2]==33);
    assert(tca9539_init(&dev)==0);assert(tca9539_set_output(&dev,1)==0);
    assert(tca9539_set_output(&dev,33)==0);assert(tca9539_set_output(&dev,0)==0);
    assert(tca9539_verify_output(&dev)==0);
}
int main(int argc,char **argv)
{
    const struct {const char *name;void (*fn)(void);} cases[]={
        {"sequence",sequence},{"timing",timing},{"all_ranges",all_ranges},{"stop_stages",stop_stages},
        {"interlock_stages",interlock_stages},{"io_failure",io_failure},{"stale",stale},
        {"regulation",regulation},{"limits",limits},{"open_short",open_short},{"continuous",continuous},
        {"rms_scaling",rms_scaling},{"rms_clipping",rms_clipping},{"relay_mutex",relay_mutex}};
    if(argc==2) for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) if(!strcmp(argv[1],cases[i].name)) {cases[i].fn();return 0;}
    return 1;
}
