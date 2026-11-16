/* @brief 按明纬DRS手册Modbus表读取；不提供任何电源写命令。 */
#include "core/drs.h"

static const struct { uint8_t function; uint16_t address; } registers[] = {
    [DRS_FAULT] = {3, 0x0040},
    [DRS_CHARGE] = {3, 0x00b8},
    [DRS_SYSTEM] = {3, 0x00c3},
    [DRS_BATTERY] = {4, 0x00d3}
};
static uint16_t crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xa001u : 0u);
    }
    return crc;
}
static bool valid(uint8_t address, enum drs_register reg)
{
    return address >= 0x80 && address <= 0x83 && (unsigned int)reg < DRS_REGISTER_COUNT;
}
int drs_request(uint8_t address, enum drs_register reg, uint8_t frame[8])
{
    if (!frame || !valid(address, reg)) return -1;
    uint16_t addr = registers[reg].address;
    frame[0] = address; frame[1] = registers[reg].function;
    frame[2] = (uint8_t)(addr >> 8); frame[3] = (uint8_t)addr;
    frame[4] = 0; frame[5] = 1;
    uint16_t crc = crc16(frame, 6);
    frame[6] = (uint8_t)crc; frame[7] = (uint8_t)(crc >> 8);
    return 0;
}
int drs_reply(uint8_t address, enum drs_register reg, const uint8_t *frame,
              size_t size, uint16_t *value)
{
    if (!frame || !value || !valid(address, reg) || (size != 5 && size != 7) ||
        frame[0] != address) return 0;
    uint16_t crc = crc16(frame, size-2);
    if (frame[size-2] != (uint8_t)crc || frame[size-1] != (uint8_t)(crc >> 8)) return 0;
    if (size == 5 && frame[1] == (registers[reg].function | 0x80u) && frame[2] != 0)
        return -(int)frame[2];
    if (size != 7 || frame[1] != registers[reg].function || frame[2] != 2) return 0;
    *value = (uint16_t)(((uint16_t)frame[3] << 8) | frame[4]);
    return 1;
}
void drs_store(struct drs_snapshot *state, enum drs_register reg, uint16_t value)
{
    if (!state) return;
    switch (reg) {
    case DRS_FAULT: state->fault = value; break;
    case DRS_CHARGE: state->charge = value; break;
    case DRS_SYSTEM: state->system = value; break;
    case DRS_BATTERY: state->battery_raw = value; break;
    default: break;
    }
}
enum drs_battery drs_battery_state(const struct drs_snapshot *state, int64_t now)
{
    if (!state || !state->valid || now < state->time_ms || now-state->time_ms >= 2000 ||
        !(state->system & (1u << 5)) || (state->system & (1u << 6)))
        return DRS_BATTERY_UNKNOWN;
    /* CHG_STATUS: BTNC=bit11，充电/后备超时=bit12..15；不推算SOC。 */
    return state->charge & 0xf800u ? DRS_BATTERY_ALARM : DRS_BATTERY_PRESENT;
}
