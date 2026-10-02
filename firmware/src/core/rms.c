/* @brief 整数累积避免小信号精度损失，窗口收尾仅两次平方根。 */
#include "core/rms.h"
#include "config/analog.h"
#include <math.h>

void rms_reset(struct rms *r) { *r = (struct rms){0}; }

int rms_add(struct rms *r, const uint32_t *samples, size_t count)
{
    if (!r || !samples || count == 0 || count > 8500 || r->count + count > 8500) return -1;
    for (size_t n = 0; n < count; ++n) {
        for (unsigned int ch = 0; ch < 2; ++ch) {
            uint32_t code = (samples[n] >> (ch * 16)) & 0xffffu;
            if (code < 10 || code > 4085) return -1;
            r->sum[ch] += code;
            r->squares[ch] += (uint64_t)code * code;
        }
    }
    r->count += (uint32_t)count;
    return 0;
}

int rms_result(const struct rms *r, struct power_reading *out)
{
    if (!r || !out || r->count != 8500) return -1;
    float code[2];
    for (unsigned int ch = 0; ch < 2; ++ch) {
        if (r->sum[ch] < (uint64_t)1792 * r->count || r->sum[ch] > (uint64_t)2304 * r->count) return -1;
        uint64_t square = r->squares[ch] * r->count;
        uint64_t mean = r->sum[ch] * r->sum[ch];
        if (mean > square) return -1;
        code[ch] = sqrtf((float)(square - mean)) / (float)r->count;
    }
    float mv = code[0] * (2.0f * HT_VREF_MV / 4095.0f) *
        (HT_DIV_HIGH_OHM + HT_DIV_LOW_OHM) / (2.0f * HT_DIV_LOW_OHM);
    float ma = code[1] * (2.0f * HT_VREF_MV / 4095.0f) * 1000000.0f / (8.2f * HT_SHUNT_UOHM);
    if (!isfinite(mv) || !isfinite(ma) || mv > 1000000 || ma > 50000) return -1;
    out->voltage_mv = (uint32_t)(mv + 0.5f);
    out->current_ma = (uint32_t)(ma + 0.5f);
    out->apparent_mva = (uint32_t)((uint64_t)out->voltage_mv * out->current_ma / 1000u);
    out->valid = true;
    return 0;
}
