/* @brief DRS-240 Modbus只读帧与状态解析，不依赖串口或调度器。 */
#ifndef HT_DRS_H
#define HT_DRS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum drs_register { DRS_FAULT, DRS_CHARGE, DRS_SYSTEM, DRS_BATTERY, DRS_REGISTER_COUNT };
enum drs_battery { DRS_BATTERY_UNKNOWN, DRS_BATTERY_PRESENT, DRS_BATTERY_ALARM };
struct drs_snapshot {
    uint16_t fault, charge, system, battery_raw;
    uint8_t address;
    uint32_t sequence;
    int64_t time_ms;
    int error;
    bool valid;
};
int drs_request(uint8_t address, enum drs_register reg, uint8_t frame[8]);
/* 1=完整数据帧，0=无有效匹配帧，负值=Modbus异常码的相反数。 */
int drs_reply(uint8_t address, enum drs_register reg, const uint8_t *frame,
              size_t size, uint16_t *value);
void drs_store(struct drs_snapshot *state, enum drs_register reg, uint16_t value);
enum drs_battery drs_battery_state(const struct drs_snapshot *state, int64_t now);
#endif
