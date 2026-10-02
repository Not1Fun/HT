/* @brief 验证三页VP契约、温度与事件边界、清尾和发送重建。 */
#include "view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define BASE_FRAME_COUNT (VIEW_FIELD_COUNT + 2u)
#define FRAME_COUNT (BASE_FRAME_COUNT + 10u)
struct capture {
    enum dgus_crc crc;
    size_t count, attempts, fail_at;
    struct dgus_frame frames[FRAME_COUNT];
};
static int receive(void *ctx, const uint8_t *data, size_t length)
{
    struct capture *c = ctx;
    if (c->attempts++ == c->fail_at) return -1;
    CHECK(c->count < FRAME_COUNT);
    CHECK(dgus_decode(c->crc, data, length, &c->frames[c->count]) == DGUS_OK);
    CHECK(c->frames[c->count].kind == DGUS_WRITE);
    ++c->count;
    return 0;
}
static struct view_snapshot sample(void)
{
    struct view_snapshot s = {.page=PANEL_PAGE_STATUS, .state=VIEW_RUNNING,
        .battery=VIEW_POWER_NORMAL, .selected=PANEL_FREQUENCY, .output=VIEW_OUTPUT_RUNNING, .fresh=true};
    const int64_t values[] = {2240, 22360, 10, 2000, 3723, 5000, 251, -125, 1200, 25000};
    _Static_assert(sizeof(values)/sizeof(values[0]) == VIEW_FIELD_COUNT, "Sample must cover every VP field");
    for (size_t i=0; i<VIEW_FIELD_COUNT; ++i) s.values[i]=(struct view_value){values[i],true};
    return s;
}
static void refresh(struct view *v, struct capture *c, const struct view_snapshot *s, enum dgus_crc crc)
{
    *c = (struct capture){.crc=crc, .fail_at=SIZE_MAX};
    CHECK(view_refresh(v,s,crc,receive,c)==VIEW_OK);
}
static void text_is(const struct capture *c, enum view_field field, const char *expected)
{
    const struct dgus_frame *f=&c->frames[1u+field];
    char text[VIEW_TEXT_BYTES];
    static const uint16_t addresses[] = {0x1100,0x1110,0x1120,0x1130,0x1140,
        0x1160,0x1170,0x1180,0x1190,0x11a0};
    _Static_assert(sizeof(addresses)/sizeof(addresses[0]) == VIEW_FIELD_COUNT, "Keep all physical VPs fixed");
    CHECK(f->vp==addresses[field] && f->count==16);
    for(size_t i=0;i<16;++i) {
        text[2*i]=(char)(f->words[i]>>8);
        text[2*i+1]=(char)f->words[i];
    }
    CHECK(memcmp(text,expected,strlen(expected))==0);
    for(size_t i=strlen(expected);i<sizeof(text);++i) CHECK(text[i]==0);
}
static void test_mapping(void)
{
    const char *expected[]={"2.240","22.360","10","2","01:02:03","5","25.1","-12.5","120.0","25"};
    _Static_assert(sizeof(expected)/sizeof(expected[0]) == VIEW_FIELD_COUNT, "Expected text must cover every VP field");
    for(int crc=DGUS_CRC_NONE;crc<=DGUS_CRC_MODBUS;++crc) {
        struct view v={0}; struct capture c; struct view_snapshot s=sample();
        s.selected=PANEL_FREQUENCY; s.editing=true;
        refresh(&v,&c,&s,(enum dgus_crc)crc);
        CHECK(c.count==BASE_FRAME_COUNT && v.synced);
        CHECK(c.frames[0].vp==0x1000 && c.frames[0].count==5);
        CHECK(c.frames[0].words[0]==1 && c.frames[0].words[1]==0);
        CHECK(c.frames[0].words[2]==0 && c.frames[0].words[3]==1);
        CHECK(c.frames[0].words[4]==VIEW_OUTPUT_RUNNING);
        for(int i=0;i<VIEW_FIELD_COUNT;++i) text_is(&c,(enum view_field)i,expected[i]);
        CHECK(c.frames[BASE_FRAME_COUNT-1].vp==0x0084 && c.frames[BASE_FRAME_COUNT-1].count==2);
        CHECK(c.frames[BASE_FRAME_COUNT-1].words[0]==0x5a01 && c.frames[BASE_FRAME_COUNT-1].words[1]==0);
    }
}
static void test_output_states(void)
{
    for(int crc=DGUS_CRC_NONE;crc<=DGUS_CRC_MODBUS;++crc) {
        struct view v={0}; struct capture c; struct view_snapshot s=sample();
        s.page=PANEL_PAGE_SETTINGS; s.selected=PANEL_OUTPUT;
        for(int output=VIEW_OUTPUT_OFF;output<=VIEW_OUTPUT_DISABLED;++output) {
            s.output=(enum view_output)output;
            s.editing=output==VIEW_OUTPUT_ARMED;
            refresh(&v,&c,&s,(enum dgus_crc)crc);
            CHECK(c.frames[0].vp==0x1000 && c.frames[0].count==5);
            CHECK(c.frames[0].words[2]==2);
            CHECK(c.frames[0].words[3]==(output==VIEW_OUTPUT_ARMED?1u:0u));
            CHECK(c.frames[0].words[4]==(uint16_t)output);
            text_is(&c,VIEW_FREQUENCY,"2");
            text_is(&c,VIEW_FREQUENCY_CHOICE,"5");
        }
        /* 草稿与实际输出分别传送，不能因状态RUNNING或修改候选频率而变成已开启。 */
        s.output=VIEW_OUTPUT_ARMED; s.editing=true;
        s.values[VIEW_FREQUENCY_CHOICE].value=10000;
        refresh(&v,&c,&s,(enum dgus_crc)crc);
        CHECK(c.frames[0].words[0]==VIEW_RUNNING && c.frames[0].words[4]==VIEW_OUTPUT_ARMED);
        text_is(&c,VIEW_FREQUENCY,"2"); text_is(&c,VIEW_FREQUENCY_CHOICE,"10");
        s.output=VIEW_OUTPUT_FAULT; s.state=VIEW_FAULT; s.editing=false;
        refresh(&v,&c,&s,(enum dgus_crc)crc);
        CHECK(c.frames[0].words[0]==VIEW_FAULT && c.frames[0].words[4]==VIEW_OUTPUT_FAULT);
        CHECK(c.frames[0].words[3]==0);
        text_is(&c,VIEW_CURRENT,"--"); text_is(&c,VIEW_NTC1,"25.1");
    }
}
static void test_output_stale(void)
{
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    for(int output=VIEW_OUTPUT_OFF;output<=VIEW_OUTPUT_DISABLED;++output) {
        s.output=(enum view_output)output;
        for(int state=VIEW_STANDBY;state<=VIEW_OFFLINE;++state) {
            s.state=(enum view_state)state; s.fresh=false;
            refresh(&v,&c,&s,DGUS_CRC_NONE);
            CHECK(c.frames[0].words[4]==VIEW_OUTPUT_UNAVAILABLE);
            CHECK(c.frames[0].words[1]==VIEW_POWER_UNKNOWN);
            text_is(&c,VIEW_ELAPSED,"--");
        }
        s.state=VIEW_OFFLINE; s.fresh=true;
        refresh(&v,&c,&s,DGUS_CRC_MODBUS);
        CHECK(c.frames[0].words[4]==VIEW_OUTPUT_UNAVAILABLE);
    }
}
static void test_numbers(void)
{
    const int64_t values[]={0,-1,99999999,-9999999,100000000,-10000000,INT64_MIN,INT64_MAX};
    const char *expected[]={"0.000","-0.001","99999.999","-9999.999","--","--","--","--"};
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        s.values[VIEW_CURRENT].value=values[i];
        refresh(&v,&c,&s,DGUS_CRC_NONE);
        text_is(&c,VIEW_CURRENT,expected[i]);
    }
    s.values[VIEW_CURRENT].valid=false;
    s.values[VIEW_VOLTAGE].value=1;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    text_is(&c,VIEW_CURRENT,"--"); text_is(&c,VIEW_VOLTAGE,"0.001");
}
static void test_time(void)
{
    const int64_t seconds[]={0,59,60,3599,3600,359999,360000,3599999,3600000,-1,INT64_MAX};
    const char *expected[]={"00:00:00","00:00:59","00:01:00","00:59:59","01:00:00",
        "99:59:59","100:00:00","999:59:59","--","--","--"};
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    for(size_t i=0;i<sizeof(seconds)/sizeof(seconds[0]);++i) {
        s.values[VIEW_ELAPSED].value=seconds[i];
        s.page=i%2?PANEL_PAGE_SETTINGS:PANEL_PAGE_STATUS;
        refresh(&v,&c,&s,DGUS_CRC_NONE);
        text_is(&c,VIEW_ELAPSED,expected[i]);
        CHECK(s.values[VIEW_ELAPSED].value==seconds[i]);
    }
}
static void test_choices(void)
{
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    s.values[VIEW_FREQUENCY_CHOICE].value=10000;
    s.page=PANEL_PAGE_SETTINGS; s.editing=true;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    text_is(&c,VIEW_RANGE,"10"); text_is(&c,VIEW_FREQUENCY,"2");
    text_is(&c,VIEW_FREQUENCY_CHOICE,"10");
    for(size_t i=0;i<c.count;++i) CHECK(c.frames[i].vp != 0x1150);
    const int64_t bad[]={0,-1,2101,INT64_MAX};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        s.values[VIEW_FREQUENCY_CHOICE].value=bad[i]; s.values[VIEW_RANGE].value=bad[i];
        refresh(&v,&c,&s,DGUS_CRC_NONE);
        text_is(&c,VIEW_RANGE,"--"); text_is(&c,VIEW_FREQUENCY_CHOICE,"--");
    }
}
static void test_power_choice(void)
{
    const int64_t values[]={0,1000,25000,49000,50000,50001,-1,INT64_MIN,INT64_MAX};
    const char *expected[]={"0","1","25","49","50","--","--","--","--"};
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    s.page=PANEL_PAGE_SETTINGS; s.selected=PANEL_POWER; s.editing=true;
    for(int crc=DGUS_CRC_NONE;crc<=DGUS_CRC_MODBUS;++crc) {
        for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) {
            s.values[VIEW_POWER_CHOICE]=(struct view_value){values[i],true};
            refresh(&v,&c,&s,(enum dgus_crc)crc);
            CHECK(c.frames[0].words[2]==1 && c.frames[0].words[3]==1);
            CHECK(c.frames[1+VIEW_POWER_CHOICE].vp==0x11a0);
            text_is(&c,VIEW_POWER_CHOICE,expected[i]);
            text_is(&c,VIEW_CURRENT,"2.240"); text_is(&c,VIEW_VOLTAGE,"22.360");
        }
    }
    s.fresh=false; s.state=VIEW_OFFLINE; s.editing=false;
    s.values[VIEW_POWER_CHOICE]=(struct view_value){50000,true};
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    text_is(&c,VIEW_POWER_CHOICE,"50"); text_is(&c,VIEW_CURRENT,"--");
    CHECK(c.frames[0].words[3]==0);
    s.values[VIEW_POWER_CHOICE].valid=false;
    refresh(&v,&c,&s,DGUS_CRC_NONE); text_is(&c,VIEW_POWER_CHOICE,"--");
    s.values[VIEW_POWER_CHOICE]=(struct view_value){0,true};
    refresh(&v,&c,&s,DGUS_CRC_NONE); text_is(&c,VIEW_POWER_CHOICE,"0");
}
static void test_auto_range(void)
{
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    s.state=VIEW_SWITCHING; s.selected=PANEL_POWER; s.editing=true;
    s.values[VIEW_RANGE]=(struct view_value){1000,true};
    s.values[VIEW_FREQUENCY_CHOICE].value=10000;
    s.values[VIEW_POWER_CHOICE].value=5000;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    CHECK(c.frames[0].words[0]==VIEW_SWITCHING);
    text_is(&c,VIEW_RANGE,"1000"); text_is(&c,VIEW_FREQUENCY,"2");
    text_is(&c,VIEW_CURRENT,"--"); text_is(&c,VIEW_VOLTAGE,"--");
    text_is(&c,VIEW_FREQUENCY_CHOICE,"10"); text_is(&c,VIEW_POWER_CHOICE,"5");
    for(size_t i=0;i<c.count;++i) CHECK(c.frames[i].vp!=0x1150);
    s.state=VIEW_RUNNING; s.values[VIEW_RANGE].value=30;
    refresh(&v,&c,&s,DGUS_CRC_MODBUS);
    text_is(&c,VIEW_RANGE,"30"); text_is(&c,VIEW_FREQUENCY_CHOICE,"10");
    text_is(&c,VIEW_POWER_CHOICE,"5");
    s.values[VIEW_RANGE].valid=false;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    text_is(&c,VIEW_RANGE,"--");
}
static void test_temperature(void)
{
    const int64_t values[]={-201,-200,-1,0,1,251,1200,1201,INT64_MIN,INT64_MAX};
    const char *expected[]={"--","-20.0","-0.1","0.0","0.1","25.1","120.0","--","--","--"};
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        s.values[VIEW_NTC1].value=values[i];
        refresh(&v,&c,&s,DGUS_CRC_NONE);
        text_is(&c,VIEW_NTC1,expected[i]);
        text_is(&c,VIEW_NTC2,"-12.5");
    }
    s.values[VIEW_NTC1]=(struct view_value){250,true};
    s.values[VIEW_NTC2].valid=false;
    s.state=VIEW_FAULT;
    refresh(&v,&c,&s,DGUS_CRC_MODBUS);
    text_is(&c,VIEW_NTC1,"25.0"); text_is(&c,VIEW_NTC2,"--");
    s.fresh=false;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    text_is(&c,VIEW_NTC1,"--"); text_is(&c,VIEW_NTC3,"--");
}
static void vp_text_is(const struct capture *c, uint16_t vp, const char *expected)
{
    for(size_t i=0;i<c->count;++i) {
        const struct dgus_frame *f=&c->frames[i];
        if(f->vp!=vp) continue;
        CHECK(f->count==16);
        for(size_t j=0;j<VIEW_TEXT_BYTES;++j) {
            unsigned int byte=j%2 ? f->words[j/2]&255u : f->words[j/2]>>8;
            CHECK(byte==(j<strlen(expected)?(uint8_t)expected[j]:0));
        }
        return;
    }
    CHECK(0);
}
static void test_logs(void)
{
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    s.page=PANEL_PAGE_LOG; s.log_count=6; s.log_offset=1;
    s.logs[0]=(struct event_entry){3661,EVENT_RANGE,30};
    s.logs[1]=(struct event_entry){3660,EVENT_FREQUENCY,10000};
    s.logs[2]=(struct event_entry){3599,EVENT_TEMP_INVALID,2};
    s.logs[3]=(struct event_entry){0,EVENT_BATTERY_ALARM,0};
    for(int crc=DGUS_CRC_NONE;crc<=DGUS_CRC_MODBUS;++crc) {
        view_reset(&v); refresh(&v,&c,&s,(enum dgus_crc)crc);
        CHECK(c.count==FRAME_COUNT);
        CHECK(c.frames[VIEW_FIELD_COUNT+1].vp==0x1010);
        CHECK(c.frames[VIEW_FIELD_COUNT+1].words[2]==EVENT_TEMP_INVALID);
        vp_text_is(&c,0x1200,"01:01:01"); vp_text_is(&c,0x1210,"30 ohm");
        vp_text_is(&c,0x1230,"10 kHz"); vp_text_is(&c,0x1250,"NTC2");
        vp_text_is(&c,0x1270,""); vp_text_is(&c,0x1280,"02-05/06");
        CHECK(c.frames[FRAME_COUNT-1].vp==0x0084 && c.frames[FRAME_COUNT-1].words[1]==2);
    }
    s.logs[0]=(struct event_entry){UINT32_MAX,EVENT_RANGE,INT32_MAX};
    s.logs[1]=(struct event_entry){3599999,EVENT_FREQUENCY,2101};
    s.logs[2]=(struct event_entry){0,EVENT_TEMP_INVALID,4};
    s.logs[3]=(struct event_entry){1,EVENT_IO_ERROR,INT32_MIN};
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    CHECK(c.count==FRAME_COUNT-1);
    vp_text_is(&c,0x1200,"--"); vp_text_is(&c,0x1210,"--");
    vp_text_is(&c,0x1220,"999:59:59"); vp_text_is(&c,0x1230,"--");
    vp_text_is(&c,0x1250,"--"); vp_text_is(&c,0x1270,"--");
    s.logs[0]=(struct event_entry){1,EVENT_RANGE,1000};
    s.logs[1]=(struct event_entry){1,EVENT_FREQUENCY,2000};
    s.logs[2]=(struct event_entry){1,EVENT_TEMP_INIT_FAILED,-99999999};
    s.logs[3]=(struct event_entry){1,EVENT_IO_ERROR,INT32_MAX};
    s.log_count=32; s.log_offset=28;
    refresh(&v,&c,&s,DGUS_CRC_MODBUS);
    vp_text_is(&c,0x1210,"1000 ohm"); vp_text_is(&c,0x1230,"2 kHz");
    vp_text_is(&c,0x1250,"-99999999"); vp_text_is(&c,0x1270,"--");
    vp_text_is(&c,0x1280,"29-32/32");
    s.logs[0].value=1; s.logs[1].value=5000; s.logs[2].value=-5;
    s.logs[3]=(struct event_entry){1,EVENT_TEMP_READY,1};
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    vp_text_is(&c,0x1210,"1 ohm"); vp_text_is(&c,0x1230,"5 kHz");
    vp_text_is(&c,0x1250,"-5"); vp_text_is(&c,0x1270,"NTC1");
    memset(s.logs,0,sizeof(s.logs)); s.log_count=0; s.log_offset=0;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    for(size_t row=0;row<VIEW_LOG_ROWS;++row) {
        CHECK(c.frames[VIEW_FIELD_COUNT+1].words[row]==EVENT_NONE);
        vp_text_is(&c,(uint16_t)(0x1200u+row*0x20u),"");
        vp_text_is(&c,(uint16_t)(0x1210u+row*0x20u),"");
    }
    vp_text_is(&c,0x1280,"00-00/00");
    for(size_t fail=0;fail<FRAME_COUNT;++fail) {
        view_reset(&v); c=(struct capture){.crc=DGUS_CRC_NONE,.fail_at=fail};
        CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_SEND);
        CHECK(!v.synced && c.attempts==fail+1);
        refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==FRAME_COUNT);
    }
}
static void test_dds_logs(void)
{
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    s.page=PANEL_PAGE_LOG; s.log_count=4;
    s.logs[0]=(struct event_entry){60,EVENT_DDS_START,10000};
    s.logs[1]=(struct event_entry){59,EVENT_DDS_STOP,0};
    s.logs[2]=(struct event_entry){58,EVENT_DDS_FAILED,-5};
    s.logs[3]=(struct event_entry){57,EVENT_DDS_START,2101};
    for(int crc=DGUS_CRC_NONE;crc<=DGUS_CRC_MODBUS;++crc) {
        view_reset(&v); refresh(&v,&c,&s,(enum dgus_crc)crc);
        const struct dgus_frame *icons=&c.frames[VIEW_FIELD_COUNT+1];
        CHECK(icons->vp==0x1010 && icons->count==4);
        CHECK(icons->words[0]==16 && icons->words[1]==17 && icons->words[2]==18);
        vp_text_is(&c,0x1200,"00:01:00"); vp_text_is(&c,0x1210,"10 kHz");
        vp_text_is(&c,0x1230,""); vp_text_is(&c,0x1250,"-5"); vp_text_is(&c,0x1270,"--");
    }
    s.logs[0].value=2000; s.logs[2].value=INT32_MIN;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    vp_text_is(&c,0x1210,"2 kHz"); vp_text_is(&c,0x1250,"--");
    memset(s.logs,0,sizeof(s.logs)); s.log_count=0;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    for(size_t row=0;row<VIEW_LOG_ROWS;++row) {
        CHECK(c.frames[VIEW_FIELD_COUNT+1].words[row]==EVENT_NONE);
        vp_text_is(&c,(uint16_t)(0x1210u+row*0x20u),"");
    }
}
static void test_invalid_readings(void)
{
    struct view v={0}; struct capture c; struct view_snapshot s=sample();
    for(int state=VIEW_STANDBY;state<=VIEW_OFFLINE;++state) {
        s.state=(enum view_state)state;
        refresh(&v,&c,&s,DGUS_CRC_NONE);
        text_is(&c,VIEW_CURRENT,state>=VIEW_SWITCHING?"--":"2.240");
        text_is(&c,VIEW_VOLTAGE,state>=VIEW_SWITCHING?"--":"22.360");
        text_is(&c,VIEW_ELAPSED,state==VIEW_OFFLINE?"--":"01:02:03");
        CHECK(c.frames[0].words[1]==(state==VIEW_OFFLINE?VIEW_POWER_UNKNOWN:VIEW_POWER_NORMAL));
    }
    s.state=VIEW_RUNNING; s.fresh=false;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    for(int i=0;i<=VIEW_ELAPSED;++i) text_is(&c,(enum view_field)i,"--");
    text_is(&c,VIEW_FREQUENCY_CHOICE,"5");
    CHECK(c.frames[0].words[0]==VIEW_OFFLINE && c.frames[0].words[1]==VIEW_POWER_UNKNOWN);
    s.state=VIEW_FAULT;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    CHECK(c.frames[0].words[0]==VIEW_FAULT);
}
static void test_transitions(void)
{
    struct view v={0}, second={0}; struct capture c; struct view_snapshot s=sample();
    refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==BASE_FRAME_COUNT);
    refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==BASE_FRAME_COUNT-1);
    s.state=VIEW_FAULT;
    refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==BASE_FRAME_COUNT-1);
    s.page=PANEL_PAGE_SETTINGS;
    refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==BASE_FRAME_COUNT && c.frames[BASE_FRAME_COUNT-1].words[1]==1);
    refresh(&second,&c,&s,DGUS_CRC_NONE); CHECK(c.count==BASE_FRAME_COUNT);
    view_reset(&v);
    refresh(&v,&c,&s,DGUS_CRC_MODBUS); CHECK(c.count==BASE_FRAME_COUNT);
    view_reset(NULL);
}
static void test_send_failure(void)
{
    struct view_snapshot s=sample();
    for(size_t fail=0;fail<BASE_FRAME_COUNT;++fail) {
        struct view v={0}; struct capture c={.crc=DGUS_CRC_NONE,.fail_at=fail};
        CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_SEND);
        CHECK(!v.synced && c.attempts==fail+1 && c.count==fail);
        refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==BASE_FRAME_COUNT);
    }
}
static void test_invalid_args(void)
{
    struct view v={0}; struct capture c={.crc=DGUS_CRC_NONE,.fail_at=SIZE_MAX};
    struct view_snapshot s=sample();
    CHECK(view_refresh(NULL,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    CHECK(view_refresh(&v,NULL,c.crc,receive,&c)==VIEW_ERR_ARG);
    CHECK(view_refresh(&v,&s,c.crc,NULL,&c)==VIEW_ERR_ARG);
    CHECK(view_refresh(&v,&s,DGUS_CRC_UNCONFIGURED,receive,&c)==VIEW_ERR_CRC);
    s.page=(enum panel_page)-1; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    s=sample(); s.state=(enum view_state)-1; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    s=sample(); s.battery=(enum view_power)3; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    s=sample(); s.selected=(enum panel_field)(PANEL_OUTPUT+1); CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    s=sample(); s.selected=(enum panel_field)-1; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    s=sample(); s.output=(enum view_output)6; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    s=sample(); s.output=(enum view_output)-1; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    s=sample(); s.log_count=EVENT_LOG_CAPACITY+1; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    s=sample(); s.log_count=6; s.log_offset=3; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    s=sample(); s.logs[0].kind=EVENT_COUNT; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    CHECK(c.attempts==0);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);
    struct { const char *name; void (*run)(void); } cases[]={
        {"mapping",test_mapping},{"numbers",test_numbers},{"time",test_time},{"choices",test_choices},
        {"power_choice",test_power_choice},{"auto_range",test_auto_range},
        {"temperature",test_temperature},{"logs",test_logs},
        {"output_states",test_output_states},{"output_stale",test_output_stale},{"dds_logs",test_dds_logs},
        {"invalid_readings",test_invalid_readings},{"transitions",test_transitions},
        {"send_failure",test_send_failure},{"invalid_args",test_invalid_args}};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        if(strcmp(argv[1],cases[i].name)==0) {cases[i].run();return 0;}
    }
    return 2;
}
