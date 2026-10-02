/* @brief 配对ADC码值累积为交流RMS和视在功率，不推定有功功率。 */
#ifndef HT_RMS_H
#define HT_RMS_H
#include "core/power.h"
#include <stddef.h>
struct rms {
    uint64_t sum[2], squares[2];
    uint32_t count;
};
void rms_reset(struct rms *rms);
int rms_add(struct rms *rms, const uint32_t *samples, size_t count);
int rms_result(const struct rms *rms, struct power_reading *reading);
#endif
