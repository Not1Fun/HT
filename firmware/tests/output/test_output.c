/* 包含真实输出线程和控制器，仅模拟外围IO与调度时钟。 */
#include "mock.h"
#include "../../src/app/output.c"
#include <string.h>
static uint32_t digital = BIT(BOARD_SENSOR)|BIT(BOARD_COIL)|BIT(BOARD_DC)|BIT(BOARD_AC);
static struct signal_snapshot measured;
static bool signal_failed, signal_active, sampling, relay_failure, fault_during_select, cancelled_select;
static bool sample_invalid;
static uint8_t stop_at;
static int observed_stops;
static int signal_error, board_error, starts;
static int sample_delay;
static int64_t sample_age, reading_after;
static uint8_t relay_value;
static uint8_t panel_raw = 0x3f;
static uint16_t amplitude;
static int clears, clear_error;
static bool old_alarm;
int relay_io_check(void) { return relay_failure ? -EIO : 0; }
int board_io_clear_protection(void) {
    assert(!signal_active && relay_value == 0 && amplitude == 0);
    ++clears;
    if (clear_error) return clear_error;
    if (old_alarm) digital &= ~(BIT(BOARD_OC) | BIT(BOARD_OV));
    return 0;
}
int board_io_read(uint32_t *value) {if(board_error)return -EIO;*value=digital;return 0;}
int signal_io_init(void) {sampling=true;return 0;}
int signal_io_stop(void) {signal_active=false;sampling=!signal_failed;amplitude=0;return 0;}
void signal_io_fault(void) {signal_failed=true;(void)signal_io_stop();}
bool signal_io_failed(void) {return signal_failed;}
int signal_io_recover(void) {
    assert(!signal_active && !relay_value);
    if(signal_error)return -EIO;
    signal_failed=false;sampling=true;reading_after=clock_time+sample_delay;return 0;
}
int signal_io_start(uint32_t hz) {
    assert(hz==2000);if(signal_failed)return -EIO;
    starts++;signal_active=true;reading_after=clock_time+sample_delay;return 0;
}
int signal_io_poll(struct signal_snapshot *s) {
    if(signal_error || signal_failed)return -EIO;
    static const uint32_t nominal[]={7070,12250,22360,38730,70700,122500,223600};
    uint32_t mv=nominal[control.range]*amplitude/2047;
    measured.reading.voltage_mv=mv;measured.reading.current_ma=mv/30;
    measured.reading.apparent_mva=(uint32_t)((uint64_t)mv*(mv/30)/1000);
    measured.reading.time_ms=clock_time-sample_age;
    if(clock_time%50==0 || !measured.reading.sequence)measured.reading.sequence++;
    measured.reading.valid=sampling && !sample_invalid && clock_time>=reading_after;
    measured.temperature_ms=clock_time;*s=measured;
    return 0;
}
int dds_io_set_amplitude(uint16_t value) {if(signal_failed)return -EIO;amplitude=value;return 0;}
int relay_io_select(uint8_t value) {
    if(relay_failure)return -EIO;
    relay_value=value;
    if(value && fault_during_select)signal_io_fault();
    if(value && value == stop_at) {stop_at=0;cancelled_select=true;output_stop();}
    return 0;
}
static void heartbeat(void) {output_inputs(panel_raw,true,true);}
static void advance(int ms) {steps=ms;if(setjmp(done)==0)run(NULL,NULL,NULL);}
static void setup(void) {
#if defined(CONFIG_HT_OUTPUT_BENCH)
    digital &= ~BIT(BOARD_COIL);
    panel_raw |= BIT(7);
#endif
    assert(output_init()==0);heartbeat();between_steps=heartbeat;advance(1);
    assert(published.available && !published.running);
}
static void start_output(void) {assert(output_start(3,2000,1000)==0);advance(4000);assert(published.running && published.range==3);}
static void stop_priority(void) {
    setup();assert(output_start(3,2000,1000)==0);output_stop();
    assert(output_start(3,2000,1000)==-EBUSY);advance(200);
    assert(starts==0 && relay_value==0 && !published.running);
    start_output();output_stop();advance(100);assert(!published.running && relay_value==0);
}
static void interlocks(void) {
    setup();const uint32_t required[]={BIT(BOARD_SENSOR),BIT(BOARD_DC)
#if !defined(CONFIG_HT_OUTPUT_BENCH)
        ,BIT(BOARD_COIL)
#endif
    };
    for(size_t i=0;i<sizeof(required)/sizeof(required[0]);i++) {digital^=required[i];advance(1);assert(!published.available);digital^=required[i];}
    const uint32_t faults[]={BIT(BOARD_OC),BIT(BOARD_OV)};
    for(size_t i=0;i<2;i++) {digital|=faults[i];advance(1);assert(!published.available);digital&=~faults[i];}
    digital &= ~BIT(BOARD_AC);advance(1);assert(!published.available);
    digital |= BIT(BOARD_BAT);advance(1);assert(published.available);
    for(int i=0;i<3;i++) {measured.temperature.samples[i].status=NTC_SATURATED;advance(1);assert(!published.available);measured.temperature.samples[i].status=NTC_OK;}
    measured.temperature.samples[0].decicelsius=800;advance(1);assert(!published.available);
    measured.temperature.samples[0].decicelsius=799;advance(1);assert(published.available);
    output_inputs(0x7f,true,true);advance(1);assert(!published.available);
    output_inputs(0xbf,true,true);advance(1);
#if defined(CONFIG_HT_OUTPUT_BENCH)
    assert(published.available);
#else
    assert(!published.available);
#endif
    output_inputs(0x3f,false,true);advance(1);assert(!published.available);
}
static void stale_ui(void) {
    setup();start_output();between_steps=NULL;int before=feeds;advance(250);
    assert(published.fault && !signal_active && relay_value==0 && starts>=1);
    assert(feeds-before<=2);int stopped_feeds=feeds;advance(100);assert(feeds==stopped_feeds);
    heartbeat();advance(1);assert(!published.available && output_start(3,2000,1000)==-EACCES);
}
static void sample_fault(void) {
    setup();start_output();signal_error=1;advance(1);
    assert(published.fault && !signal_active && amplitude==0 && relay_value==0 && published.range==POWER_RANGE_AUTO);
    assert(!published.signal.reading.valid);
}
static void standby_sampling(void) {
    setup();advance(100);
    assert(sampling && !signal_active && !published.running && starts==0 && relay_value==0);
    assert(published.signal.reading.valid && published.signal.reading.sequence>1);
}
static void standby_board_fault(void) {
    setup();advance(100);assert(published.signal.reading.valid);
    board_error=1;advance(1);
    assert(published.fault && !published.signal.reading.valid && !signal_active && relay_value==0);
}
static void init_failure(void) {
    watchdog_error=-EIO;
    assert(output_init()==-EIO);
    assert(signal_failed && !sampling && !signal_active && !published.ready && published.fault);
    assert(published.error==-EIO && wake==0);
}
static void sample_permission(void) {
    setup();sample_invalid=true;advance(100);
    assert(!published.available && !published.fault && sampling && !signal_active && relay_value==0);
    assert(output_start(3,2000,1000)==-EACCES && published.signal.temperature_ms>0);
    sample_invalid=false;sample_age=150;advance(1);
    assert(!published.available && output_start(3,2000,1000)==-EACCES);
    sample_age=-1;advance(1);
    assert(!published.available && output_start(3,2000,1000)==-EACCES);
    sample_age=149;advance(1);assert(published.available);
    sample_age=0;advance(1);assert(published.available && starts==0 && relay_value==0);
}
static void pending_sample_invalid(void) {
    setup();assert(output_start(3,2000,1000)==0);
    sample_invalid=true;advance(1);
    assert(control.state==POWER_IDLE && !published.fault && !published.available);
    assert(starts==0 && relay_value==0 && !request.pending);
    sample_invalid=false;advance(100);
    assert(published.available && control.state==POWER_IDLE && starts==0);
}
static void first_window_grace(void) {
    setup();sample_delay=60;
    start_output();
    assert(!published.fault && published.running && published.signal.reading.valid);
}
static void protection_race(void) {
    setup();fault_during_select=true;assert(output_start(3,2000,1000)==0);advance(200);
    assert(published.fault && signal_failed && starts==0 && relay_value==0);
}
static void observe_stop(void) {
    /* 在下一轮消费request.stop之前验证，旧的“下一轮再停”实现必须失败。 */
    if(cancelled_select) {
        assert(control.state==POWER_STOPPING && control.output==relay_value);
        assert(!published.running && !signal_active && amplitude==0);
        cancelled_select=false;observed_stops++;
    }
    heartbeat();
}
static void stop_in_flight(void) {
    setup();between_steps=observe_stop;
    const uint8_t ports[]={2,16};
    for(size_t i=0;i<2;i++) {
        stop_at=ports[i];assert(output_start(i == 0 ? 0 : 3,2000,1000)==0);advance(4000);
        assert(!published.fault && !published.running && relay_value==0);
        assert(observed_stops==(int)i+1);
    }
    start_output();output_stop();advance(100);assert(!published.fault && relay_value==0);
}
static void relay_fault(void) {
    setup();start_output();relay_failure=true;protection(NULL,NULL,0);int before=feeds;advance(100);
    assert(published.fault && !control.fault_stopped && !signal_active && feeds==before);
}
static void shutdown_output(void) {
    setup();start_output();output_shutdown();advance(1);
    assert(signal_failed && published.fault && relay_value==0 && output_start(3,2000,1000)==-EACCES);
}
static void enclosure_interlocks(void) {
    setup();start_output();
    digital &= ~BIT(BOARD_COIL);panel_raw |= BIT(7);heartbeat();advance(1);
#if defined(CONFIG_HT_OUTPUT_BENCH)
    assert(published.running && !published.fault && signal_active);
    output_stop();advance(200);assert(!published.running && !signal_active && relay_value==0);
    advance(200);assert(!published.running);
#else
    assert(published.fault && !signal_active && relay_value==0);
#endif
}
static void assert_tripped(void) {
    advance(1);
    assert(published.fault && !published.available && !signal_active && amplitude==0 && relay_value==0);
    assert(control.fault_stopped && output_start(3,2000,1000)==-EACCES);
    int before=feeds;advance(100);assert(feeds>before);
}
static void running_oc(void) {setup();start_output();digital|=BIT(BOARD_OC);assert_tripped();}
static void running_ov(void) {setup();start_output();digital|=BIT(BOARD_OV);assert_tripped();}
static void running_sensor_loss(void) {setup();start_output();digital&=~BIT(BOARD_SENSOR);assert_tripped();}
static void running_temperature(void) {setup();start_output();measured.temperature.samples[0].decicelsius=800;assert_tripped();}
static void running_ntc_failure(void) {setup();start_output();measured.temperature.samples[2].status=NTC_SATURATED;assert_tripped();}
static void running_enable_release(void) {setup();start_output();panel_raw|=BIT(6);heartbeat();assert_tripped();}
static void protection_irq(void) {setup();start_output();protection(NULL,NULL,0);assert(signal_failed && !signal_active && amplitude==0);assert_tripped();}

#if defined(CONFIG_HT_OUTPUT_BENCH)
static void debug_cycle(void) {
    setup();assert(output_debug(2,2000,25,false)==0);advance(200);
    assert(published.debug.busy && relay_value==2 && !signal_active);
    assert(output_start(3,2000,1000)==-EBUSY);
    assert(output_debug(2,2000,25,true)==0);advance(500);
    assert(published.debug.wave && amplitude==17 && signal_active && !published.running);
    advance(10100);assert(!published.debug.busy && relay_value==0 && amplitude==0);
    assert(output_debug(8,2000,25,true)==0);advance(50);
    assert(published.debug.trial && !published.debug.wave);
    output_stop();advance(500);
    assert(!published.debug.busy && relay_value==0 && !signal_active);
    start_output();assert(output_debug(2,2000,25,true)==-EBUSY);
}
static void debug_fault(void) {
    setup();assert(output_debug(2,2000,25,true)==0);advance(500);
    protection(NULL,NULL,0);advance(1);
    assert(published.fault && !published.debug.wave && relay_value==0 && !signal_active);
    assert(output_debug(2,2000,25,true)==-EACCES);
}
static void debug_disconnect(void) {
    setup();assert(output_debug(8,2000,25,true)==0);advance(500);
    output_inputs(panel_raw,false,true);advance(100);
    assert(!published.debug.busy && !signal_active && relay_value==0);
    advance(500);assert(!published.debug.busy);
}
static void dac_open_ntc(void) {
    setup();measured.temperature.samples[0].status=NTC_SATURATED;
    measured.temperature.samples[0].raw=NTC_ADC_MAX;advance(1);
    assert(!published.available && published.debug.dac_available);
    assert(output_start(3,2000,1000)==-EACCES);
    assert(output_debug(2,2000,25,true)==-EACCES);
    assert(output_debug(0,2000,25,false)==-EACCES);
    assert(output_debug(0,2000,25,true)==0);advance(500);
    assert(published.debug.wave && relay_value==0 && amplitude==17);
    advance(10100);assert(!signal_active && relay_value==0 && !published.debug.busy);
    assert(!published.available && published.debug.dac_available);
}
static void dac_guards(void) {
    setup();measured.temperature.samples[0].status=NTC_SATURATED;
    measured.temperature.samples[0].raw=NTC_ADC_MAX;advance(1);
    sample_invalid=true;advance(1);assert(!published.debug.dac_available);
    sample_invalid=false;advance(1);assert(published.debug.dac_available);
    measured.temperature.samples[1].status=NTC_SHORT;advance(1);
    assert(!published.debug.dac_available && output_debug(0,2000,25,true)==-EACCES);
    measured.temperature.samples[1].status=NTC_OK;
    measured.temperature.samples[1].decicelsius=800;advance(1);
    assert(!published.debug.dac_available);
    measured.temperature.samples[1].decicelsius=250;
    digital|=BIT(BOARD_OC);advance(1);assert(!published.debug.dac_available);
    digital&=~BIT(BOARD_OC);advance(1);assert(published.debug.dac_available);
    assert(output_debug(0,2000,25,true)==0);advance(500);
    output_stop();advance(100);assert(!signal_active && relay_value==0);
    assert(output_debug(0,2000,25,true)==0);advance(500);
    measured.temperature.samples[1].decicelsius=800;advance(100);
    assert(!signal_active && relay_value==0 && !published.debug.busy);
}
static void dac_relay_guard(void) {
    setup();assert(output_debug(2,2000,25,false)==0);advance(200);
    assert(relay_value==2 && !published.debug.dac_available);
    assert(output_debug(0,2000,25,true)==-EACCES);
    output_stop();advance(100);assert(published.debug.dac_available);
    assert(output_debug(0,2000,25,true)==0);advance(500);
    protection(NULL,NULL,0);advance(1);
    assert(!signal_active && relay_value==0 && !published.debug.dac_available);
}
static void dac_one_volt(void) {
    setup();assert(output_debug(2,2000,1000,true)==-EINVAL);
    assert(output_debug(0,2000,1000,false)==-EINVAL);
    assert(output_debug(0,2000,1000,true)==0);advance(500);
    assert(published.debug.wave && amplitude==706 && relay_value==0 && !published.running);
    advance(10000);assert(!signal_active && amplitude==0 && relay_value==0 && !published.debug.busy);
}
#endif
static void blocking_reasons(void) {
    setup();
    digital |= BIT(BOARD_OC) | BIT(BOARD_OV); advance(1);
    assert(published.blocked == (OUTPUT_REASON_BIT(OUTPUT_REASON_OC) | OUTPUT_REASON_BIT(OUTPUT_REASON_OV)));
    digital &= ~(BIT(BOARD_OC) | BIT(BOARD_OV));
    measured.temperature.samples[1].status = NTC_SATURATED;
    measured.temperature.samples[1].raw = NTC_ADC_MAX; advance(1);
    assert(published.blocked == OUTPUT_REASON_BIT(OUTPUT_REASON_NTC2_OPEN));
#if defined(CONFIG_HT_OUTPUT_BENCH)
    assert(published.debug.blocked == 0 && published.debug.dac_available);
#endif
    measured.temperature.samples[1].status = NTC_SHORT; advance(1);
    assert(published.blocked == OUTPUT_REASON_BIT(OUTPUT_REASON_NTC2_INVALID));
    measured.temperature.samples[1].status = NTC_OK;
    measured.temperature.samples[2].decicelsius = 800; advance(1);
    assert(published.blocked == OUTPUT_REASON_BIT(OUTPUT_REASON_NTC3_HOT));
    measured.temperature.samples[2].decicelsius = 250;
    sample_invalid = true; advance(1);
    assert(published.blocked == OUTPUT_REASON_BIT(OUTPUT_REASON_SAMPLE_STALE));
    sample_invalid = false; digital &= ~BIT(BOARD_DC); advance(1);
    assert(published.blocked == OUTPUT_REASON_BIT(OUTPUT_REASON_DC));
    digital |= BIT(BOARD_DC); advance(1); assert(published.blocked == 0);
    assert(clears == 1 && starts == 0 && relay_value == 0);
#if defined(CONFIG_HT_OUTPUT_BENCH)
    power_fail(&control, POWER_ERROR_MATCH); advance(1);
    assert(published.debug.blocked & OUTPUT_REASON_BIT(OUTPUT_REASON_FAULT));
    assert(!(published.debug.blocked & OUTPUT_REASON_BIT(OUTPUT_REASON_BUSY)));
#endif
}
static void startup_latch(void) {
    old_alarm = true; digital |= BIT(BOARD_OC) | BIT(BOARD_OV);
    setup(); assert(clears == 1 && published.blocked == 0 && starts == 0);
    digital |= BIT(BOARD_OC); advance(100);
    assert(!published.available && published.blocked == OUTPUT_REASON_BIT(OUTPUT_REASON_OC));
    assert(clears == 1 && starts == 0 && output_init() == -EALREADY);
}
static void latch_failure(void) {
    clear_error = -EIO; assert(output_init() == -EIO);
    assert(!published.ready && published.fault && clears == 1 && starts == 0 && relay_value == 0);
}
static void latch_relay_failure(void) {
    relay_failure = true; assert(output_init() == -EIO);
    assert(!published.ready && clears == 0 && starts == 0);
}
static void manual_range(void) {
    setup();
    assert(output_start(7,2000,1000)==-EINVAL);
    for (uint8_t range=0; range<7; ++range) {
        assert(output_start(range,2000,1000)==0);
        advance(180);
        assert(!control.automatic && !control.matching && control.range==range);
        assert(published.running && published.range==range && !published.matching);
        assert(relay_value==((range>=4?1u:0u)|(1u<<(range+1u))));
        output_stop(); advance(100); assert(relay_value==0 && !published.running);
    }
    assert(!published.fault);
    /* 模拟30Ω负载时仍保持手动1Ω挡，不能跳进自动匹配或自行换挡。 */
    assert(output_start(0,2000,1000)==0); advance(6000);
    assert(published.running && published.range==0 && !published.matching && !published.fault);
    assert(!control.automatic && control.changes==0 && control.rematches==0);
    output_stop(); advance(100); assert(relay_value==0 && !published.running);
}
static void automatic_range(void) {
    setup();assert(output_start(POWER_RANGE_AUTO,2000,1000)==0);advance(8000);
    assert(published.running && !published.fault && published.range==3 && control.automatic);
    assert(published.frequency==2000 && control.target_mva==1000 && relay_value==16);
    output_stop();advance(100);assert(!signal_active && relay_value==0);
}
static void clear_fault(void) {
    setup();start_output();protection(NULL,NULL,0);advance(1);
    assert(published.fault && published.clear_needed);
    int before=starts;old_alarm=true;digital|=BIT(BOARD_OC)|BIT(BOARD_OV);
    assert(output_clear()==0 && published.clearing);
    assert(output_clear()==-EBUSY && output_start(0,2000,1000)!=0);
    advance(40);assert(published.clearing && published.fault && !relay_value && !signal_active);
    advance(200);assert(!published.clearing && !published.fault && published.available);
    assert(published.clear_count==1 && published.clear_result==0 && clears==2 && starts==before);
    assert(!published.running && !relay_value && sampling && !signal_active);
    advance(1000);assert(starts==before);
    start_output();assert(output_clear()==-EBUSY);
}
static void clear_blocked(void) {
    setup();protection(NULL,NULL,0);digital|=BIT(BOARD_OC);advance(1);
    assert(output_clear()==0);advance(200);
    assert(published.fault && !published.clearing && published.clear_result==-EACCES);
    assert(clears==2 && starts==0 && !relay_value && !signal_active);
    advance(1500);assert(clears==2 && published.clear_count==1);
    digital&=~BIT(BOARD_OC);assert(output_clear()==0);advance(300);
    assert(!published.fault && published.clear_count==2 && published.clear_result==0);
}
static void clear_cancel(void) {
    setup();power_fail(&control,POWER_ERROR_OPEN);advance(1);
    assert(output_clear()==0);output_stop();advance(200);
    assert(published.fault && published.clear_result==-ECANCELED && clears==1);
    assert(output_clear()==0);advance(50);output_stop();advance(200);
    assert(published.fault && published.clear_result==-ECANCELED && clears==2);
    assert(!relay_value && !signal_active && starts==0);
    assert(output_clear()==0);advance(50);output_shutdown();advance(200);
    assert(published.fault && output_clear()==-EACCES && starts==0);
}
static void clear_sample(void) {
    setup();power_fail(&control,POWER_ERROR_OPEN);advance(1);
    sample_invalid=true;assert(output_clear()==0);advance(1100);
    assert(published.fault && published.clear_result==-ETIMEDOUT && starts==0);
    sample_invalid=false;panel_raw|=BIT(6);heartbeat();
    assert(output_clear()==0);advance(300);
    assert(!published.fault && !published.available && published.clear_result==0);
    assert(published.blocked==OUTPUT_REASON_BIT(OUTPUT_REASON_ENABLE));
}
static void clear_io(void) {
    setup();power_fail(&control,POWER_ERROR_OPEN);advance(1);relay_failure=true;
    assert(output_clear()==0);advance(200);
    assert(published.fault && published.clear_result==-EIO && clears==1);
    relay_failure=false;signal_error=1;assert(output_clear()==0);advance(200);
    assert(published.fault && published.clear_result==-EIO && clears==2);
    signal_error=0;assert(output_clear()==0);advance(80);protection(NULL,NULL,0);advance(200);
    assert(published.fault && published.clear_result==-EIO && !signal_active && !relay_value);
    assert(output_clear()==0);advance(300);assert(!published.fault && starts==0);
}
int main(int argc,char **argv) {
    assert(argc==2);
#define RUN(name) if(!strcmp(argv[1],#name)){name();return 0;}
    RUN(clear_fault) RUN(clear_blocked) RUN(clear_cancel) RUN(clear_sample) RUN(clear_io)
    RUN(automatic_range)
#if defined(CONFIG_HT_OUTPUT_BENCH)
    RUN(debug_cycle) RUN(debug_fault) RUN(debug_disconnect)
    RUN(dac_one_volt)
    RUN(dac_open_ntc) RUN(dac_guards) RUN(dac_relay_guard)
#endif
    RUN(manual_range) RUN(blocking_reasons) RUN(startup_latch) RUN(latch_failure) RUN(latch_relay_failure)
    RUN(stop_priority) RUN(interlocks) RUN(stale_ui) RUN(sample_fault)
    RUN(standby_sampling) RUN(standby_board_fault) RUN(init_failure)
    RUN(sample_permission) RUN(pending_sample_invalid) RUN(first_window_grace)
    RUN(protection_race) RUN(stop_in_flight) RUN(relay_fault) RUN(shutdown_output)
    RUN(enclosure_interlocks) RUN(running_oc) RUN(running_ov) RUN(running_sensor_loss)
    RUN(running_temperature) RUN(running_ntc_failure) RUN(running_enable_release) RUN(protection_irq)
    return 1;
}
