/* @brief 验证两页VP契约、运行时间、候选隔离、失效与发送重建。 */
#include "view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define FRAME_COUNT 9u
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
        .battery=VIEW_POWER_NORMAL, .selected=PANEL_RANGE, .fresh=true};
    const int64_t values[] = {2240, 22360, 10, 2000, 3723, 30, 5000};
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
    CHECK(f->vp==0x1100u+(unsigned int)field*0x10u && f->count==16);
    for(size_t i=0;i<16;++i) {
        text[2*i]=(char)(f->words[i]>>8);
        text[2*i+1]=(char)f->words[i];
    }
    CHECK(memcmp(text,expected,strlen(expected))==0);
    for(size_t i=strlen(expected);i<sizeof(text);++i) CHECK(text[i]==0);
}
static void test_mapping(void)
{
    const char *expected[]={"2.240","22.360","10","2","01:02:03","30","5"};
    for(int crc=DGUS_CRC_NONE;crc<=DGUS_CRC_MODBUS;++crc) {
        struct view v={0}; struct capture c; struct view_snapshot s=sample();
        s.selected=PANEL_FREQUENCY; s.editing=true;
        refresh(&v,&c,&s,(enum dgus_crc)crc);
        CHECK(c.count==9 && v.synced);
        CHECK(c.frames[0].vp==0x1000 && c.frames[0].count==4);
        CHECK(c.frames[0].words[0]==1 && c.frames[0].words[1]==0);
        CHECK(c.frames[0].words[2]==1 && c.frames[0].words[3]==1);
        for(int i=0;i<VIEW_FIELD_COUNT;++i) text_is(&c,(enum view_field)i,expected[i]);
        CHECK(c.frames[8].vp==0x0084 && c.frames[8].count==2);
        CHECK(c.frames[8].words[0]==0x5a01 && c.frames[8].words[1]==0);
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
    s.values[VIEW_RANGE_CHOICE].value=1000; s.values[VIEW_FREQUENCY_CHOICE].value=10000;
    s.page=PANEL_PAGE_SETTINGS; s.editing=true;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    text_is(&c,VIEW_RANGE,"10"); text_is(&c,VIEW_FREQUENCY,"2");
    text_is(&c,VIEW_RANGE_CHOICE,"1000"); text_is(&c,VIEW_FREQUENCY_CHOICE,"10");
    const int64_t bad[]={0,-1,2101,INT64_MAX};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        s.values[VIEW_FREQUENCY_CHOICE].value=bad[i]; s.values[VIEW_RANGE_CHOICE].value=bad[i];
        refresh(&v,&c,&s,DGUS_CRC_NONE);
        text_is(&c,VIEW_RANGE_CHOICE,"--"); text_is(&c,VIEW_FREQUENCY_CHOICE,"--");
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
    text_is(&c,VIEW_RANGE_CHOICE,"30"); text_is(&c,VIEW_FREQUENCY_CHOICE,"5");
    CHECK(c.frames[0].words[0]==VIEW_OFFLINE && c.frames[0].words[1]==VIEW_POWER_UNKNOWN);
    s.state=VIEW_FAULT;
    refresh(&v,&c,&s,DGUS_CRC_NONE);
    CHECK(c.frames[0].words[0]==VIEW_FAULT);
}
static void test_transitions(void)
{
    struct view v={0}, second={0}; struct capture c; struct view_snapshot s=sample();
    refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==9);
    refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==8);
    s.state=VIEW_FAULT;
    refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==8);
    s.page=PANEL_PAGE_SETTINGS;
    refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==9 && c.frames[8].words[1]==1);
    refresh(&second,&c,&s,DGUS_CRC_NONE); CHECK(c.count==9);
    view_reset(&v);
    refresh(&v,&c,&s,DGUS_CRC_MODBUS); CHECK(c.count==9);
    view_reset(NULL);
}
static void test_send_failure(void)
{
    struct view_snapshot s=sample();
    for(size_t fail=0;fail<FRAME_COUNT;++fail) {
        struct view v={0}; struct capture c={.crc=DGUS_CRC_NONE,.fail_at=fail};
        CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_SEND);
        CHECK(!v.synced && c.attempts==fail+1 && c.count==fail);
        refresh(&v,&c,&s,DGUS_CRC_NONE); CHECK(c.count==9);
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
    s=sample(); s.selected=(enum panel_field)2; CHECK(view_refresh(&v,&s,c.crc,receive,&c)==VIEW_ERR_ARG);
    CHECK(c.attempts==0);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);
    struct { const char *name; void (*run)(void); } cases[]={
        {"mapping",test_mapping},{"numbers",test_numbers},{"time",test_time},{"choices",test_choices},
        {"invalid_readings",test_invalid_readings},{"transitions",test_transitions},
        {"send_failure",test_send_failure},{"invalid_args",test_invalid_args}};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        if(strcmp(argv[1],cases[i].name)==0) {cases[i].run();return 0;}
    }
    return 2;
}
