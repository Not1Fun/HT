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
static uint16_t amplitude;
int board_io_read(uint32_t *value) {if(board_error)return -EIO;*value=digital;return 0;}
int signal_io_init(void) {sampling=true;return 0;}
int signal_io_stop(void) {signal_active=false;sampling=!signal_failed;amplitude=0;return 0;}
void signal_io_fault(void) {signal_failed=true;(void)signal_io_stop();}
bool signal_io_failed(void) {return signal_failed;}
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
static void heartbeat(void) {output_inputs(0x3f,true,true);}
static void advance(int ms) {steps=ms;if(setjmp(done)==0)run(NULL,NULL,NULL);}
static void setup(void) {
    assert(output_init()==0);heartbeat();between_steps=heartbeat;advance(1);
    assert(published.available && !published.running);
}
static void start_output(void) {assert(output_start(2000,1000)==0);advance(4000);assert(published.running && published.range==3);}
static void stop_priority(void) {
    setup();assert(output_start(2000,1000)==0);output_stop();
    assert(output_start(2000,1000)==-EBUSY);advance(200);
    assert(starts==0 && relay_value==0 && !published.running);
    start_output();output_stop();advance(100);assert(!published.running && relay_value==0);
}
static void interlocks(void) {
    setup();const uint32_t required[]={BIT(BOARD_SENSOR),BIT(BOARD_COIL),BIT(BOARD_DC)};
    for(size_t i=0;i<3;i++) {digital^=required[i];advance(1);assert(!published.available);digital^=required[i];}
    digital &= ~BIT(BOARD_AC);advance(1);assert(!published.available);
    digital |= BIT(BOARD_BAT);advance(1);assert(published.available);
    for(int i=0;i<3;i++) {measured.temperature.samples[i].status=NTC_SATURATED;advance(1);assert(!published.available);measured.temperature.samples[i].status=NTC_OK;}
    measured.temperature.samples[0].decicelsius=800;advance(1);assert(!published.available);
    measured.temperature.samples[0].decicelsius=799;advance(1);assert(published.available);
    output_inputs(0x7f,true,true);advance(1);assert(!published.available);
    output_inputs(0xbf,true,true);advance(1);assert(!published.available);
    output_inputs(0x3f,false,true);advance(1);assert(!published.available);
}
static void stale_ui(void) {
    setup();start_output();between_steps=NULL;int before=feeds;advance(250);
    assert(published.fault && !signal_active && relay_value==0 && starts>=1);
    assert(feeds-before<=2);int stopped_feeds=feeds;advance(100);assert(feeds==stopped_feeds);
    heartbeat();advance(1);assert(!published.available && output_start(2000,1000)==-EACCES);
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
    assert(output_start(2000,1000)==-EACCES && published.signal.temperature_ms>0);
    sample_invalid=false;sample_age=150;advance(1);
    assert(!published.available && output_start(2000,1000)==-EACCES);
    sample_age=-1;advance(1);
    assert(!published.available && output_start(2000,1000)==-EACCES);
    sample_age=149;advance(1);assert(published.available);
    sample_age=0;advance(1);assert(published.available && starts==0 && relay_value==0);
}
static void pending_sample_invalid(void) {
    setup();assert(output_start(2000,1000)==0);
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
    setup();fault_during_select=true;assert(output_start(2000,1000)==0);advance(200);
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
        stop_at=ports[i];assert(output_start(2000,1000)==0);advance(4000);
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
    assert(signal_failed && published.fault && relay_value==0 && output_start(2000,1000)==-EACCES);
}
int main(int argc,char **argv) {
    assert(argc==2);
#define RUN(name) if(!strcmp(argv[1],#name)){name();return 0;}
    RUN(stop_priority) RUN(interlocks) RUN(stale_ui) RUN(sample_fault)
    RUN(standby_sampling) RUN(standby_board_fault) RUN(init_failure)
    RUN(sample_permission) RUN(pending_sample_invalid) RUN(first_window_grace)
    RUN(protection_race) RUN(stop_in_flight) RUN(relay_fault) RUN(shutdown_output)
    return 1;
}
