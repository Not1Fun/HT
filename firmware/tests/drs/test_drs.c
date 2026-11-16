#include "core/drs.h"
#include <assert.h>
#include <string.h>
static void crc(uint8_t *f,size_t n) {
    unsigned int c=65535;
    for(size_t i=0;i<n-2;++i) {
        c^=f[i];
        for(int b=0;b<8;++b)c=c&1?(c>>1)^40961:c>>1;
    }
    f[n-2]=(uint8_t)c;f[n-1]=(uint8_t)(c>>8);
}
int main(void) {
    uint8_t f[8];const uint8_t regs[]={0x40,0xb8,0xc3,0xd3};
    for(int reg=0;reg<DRS_REGISTER_COUNT;++reg) for(int addr=0x80;addr<=0x83;++addr) {
        assert(drs_request((uint8_t)addr,(enum drs_register)reg,f)==0);
        assert(f[0]==addr && f[1]==(reg==DRS_BATTERY?4:3) && f[2]==0 && f[3]==regs[reg] && f[4]==0 && f[5]==1);
        uint8_t expected[8];memcpy(expected,f,8);crc(expected,8);assert(!memcmp(f,expected,8));
        uint8_t reply[]={ (uint8_t)addr,f[1],2,0xa5,0x81,0,0 };crc(reply,7);
        uint16_t value=123;
        assert(drs_reply((uint8_t)addr,(enum drs_register)reg,reply,7,&value)==1 && value==0xa581);
        for(size_t i=0;i<7;++i) {
            reply[i]^=1;value=123;
            assert(drs_reply((uint8_t)addr,(enum drs_register)reg,reply,7,&value)==0 && value==123);reply[i]^=1;
        }
        reply[2]=1;crc(reply,7);assert(drs_reply((uint8_t)addr,(enum drs_register)reg,reply,7,&value)==0);
        uint8_t error[]={(uint8_t)addr,(uint8_t)(f[1]|0x80),2,0,0};crc(error,5);
        assert(drs_reply((uint8_t)addr,(enum drs_register)reg,error,5,&value)==-2);
    }
    assert(drs_request(0,DRS_FAULT,f)<0 && drs_request(0x83,DRS_REGISTER_COUNT,f)<0);
    assert(drs_request(0x83,DRS_FAULT,NULL)<0);
    struct drs_snapshot s={.valid=true,.time_ms=100,.system=32};
    assert(drs_battery_state(&s,100)==DRS_BATTERY_PRESENT);
    s.charge=1u<<11;assert(drs_battery_state(&s,100)==DRS_BATTERY_ALARM);
    s.charge=1u<<15;assert(drs_battery_state(&s,100)==DRS_BATTERY_ALARM);
    s.charge=0;assert(drs_battery_state(&s,2099)==DRS_BATTERY_PRESENT);
    assert(drs_battery_state(&s,2100)==DRS_BATTERY_UNKNOWN && drs_battery_state(&s,99)==DRS_BATTERY_UNKNOWN);
    s.system=0;assert(drs_battery_state(&s,100)==DRS_BATTERY_UNKNOWN);
    s.system=32|64;assert(drs_battery_state(&s,100)==DRS_BATTERY_UNKNOWN);
    s.valid=false;assert(drs_battery_state(&s,100)==DRS_BATTERY_UNKNOWN);
    drs_store(&s,DRS_BATTERY,3600);assert(s.battery_raw==3600);
    drs_store(&s,DRS_FAULT,4);assert(s.fault==4);
    return 0;
}
