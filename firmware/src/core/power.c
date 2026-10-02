/* @brief 静音后先断后合，按真实VA反馈积分调幅；停止/故障取消后续合闸。 */
#include "core/power.h"
#include <stddef.h>

static const uint32_t volts[] = {7070, 12250, 22360, 38730, 70700, 122500, 223600};
static const uint32_t amps[] = {7070, 4080, 2240, 1290, 710, 408, 224};
static const uint32_t boundaries[] = {1732, 5477, 17321, 54772, 173205, 547723}; /* mΩ */

static uint8_t closest(uint32_t impedance)
{
    uint8_t range = 0;
    while (range < 6 && impedance > boundaries[range]) ++range;
    return range;
}

static uint8_t stable_range(uint8_t range, uint32_t impedance)
{
    if ((range < 6 && impedance > boundaries[range] * 12u / 10u) ||
        (range > 0 && impedance < boundaries[range - 1u] * 10u / 12u)) return closest(impedance);
    return range;
}

int power_init(struct power *p, const struct power_ops *ops)
{
    if (!p || !ops || !ops->mute || !ops->select || !ops->start || !ops->level || !ops->now) return -1;
    *p = (struct power){.ops = *ops};
    return 0;
}

void power_fail(struct power *p, enum power_error error)
{
    if (p->state == POWER_FAULT) return;
    int rc = p->ops.mute(p->ops.ctx);
    int off = p->ops.select(p->ops.ctx, 0);
    if (off == 0) p->output = 0;
    p->fault_stopped = rc == 0 && off == 0;
    p->amplitude = 0;
    p->level_milli = 0;
    p->error = error;
    p->state = POWER_FAULT;
}

static bool cancel(struct power *p)
{
    if (p->state == POWER_STOPPING || p->state == POWER_RELEASE || !p->ops.cancelled ||
        !p->ops.cancelled(p->ops.ctx)) return false;
    power_stop(p, p->ops.now(p->ops.ctx));
    return true;
}

static bool select_output(struct power *p, uint8_t output)
{
    if (p->ops.select(p->ops.ctx, output) != 0) {
        power_fail(p, POWER_ERROR_IO);
        return false;
    }
    p->output = output;
    p->deadline = p->ops.now(p->ops.ctx) + 30;
    return !cancel(p);
}

int power_start(struct power *p, uint8_t range, uint32_t frequency, uint32_t target, int64_t now)
{
    (void)now;
    if (p->state != POWER_IDLE || (range >= 7 && range != POWER_RANGE_AUTO) || target == 0 || target > 50000 ||
        (frequency != 2000 && frequency != 5000 && frequency != 8000 && frequency != 10000)) return -1;
    if (p->ops.cancelled && p->ops.cancelled(p->ops.ctx)) return -1;
    p->automatic = range == POWER_RANGE_AUTO;
    p->matching = p->automatic;
    p->range = p->automatic ? 0 : range;
    p->changes = p->rematches = p->stable = 0;
    p->ramp_base = 0;
    p->session_ms = p->ops.now(p->ops.ctx);
    p->match_deadline = p->session_ms + 15000;
    p->frequency = frequency;
    p->target_mva = target;
    p->amplitude = 0;
    p->level_milli = 0;
    if (p->ops.mute(p->ops.ctx) != 0) { power_fail(p, POWER_ERROR_IO); return -1; }
    p->state = POWER_MUTING;
    p->deadline = p->ops.now(p->ops.ctx) + 30;
    (void)cancel(p);
    return 0;
}

void power_stop(struct power *p, int64_t now)
{
    (void)now;
    if (p->state == POWER_IDLE || p->state == POWER_FAULT ||
        p->state == POWER_STOPPING || p->state == POWER_RELEASE) return;
    if (p->ops.mute(p->ops.ctx) != 0) { power_fail(p, POWER_ERROR_IO); return; }
    p->amplitude = 0;
    p->level_milli = 0;
    p->state = POWER_STOPPING;
    p->deadline = p->ops.now(p->ops.ctx) + 30;
}

static void switch_range(struct power *p, uint8_t range)
{
    if (++p->changes > 10) { power_fail(p, POWER_ERROR_MATCH); return; }
    if (p->ops.mute(p->ops.ctx) != 0) { power_fail(p, POWER_ERROR_IO); return; }
    p->range = range;
    p->amplitude = p->ramp_base = p->stable = 0;
    p->level_milli = 0;
    p->matching = true;
    p->state = POWER_MUTING;
    p->deadline = p->ops.now(p->ops.ctx) + 30;
    (void)cancel(p);
}

static bool consistent(struct power *p, uint8_t candidate)
{
    if (!p->stable || candidate != p->candidate) { p->candidate = candidate; p->stable = 1; }
    else if (p->stable < 3) ++p->stable;
    return p->stable == 3;
}

static uint32_t maximum(uint32_t a, uint32_t b) { return a > b ? a : b; }

static void probe(struct power *p, const struct power_reading *r, int64_t now)
{
    uint32_t min_mv = maximum(800, p->noise_mv * 4u);
    uint32_t min_ma = maximum(20, p->noise_ma * 4u);
    uint32_t margin_mv = maximum(200, p->noise_mv * 2u);
    uint32_t margin_ma = maximum(5, p->noise_ma * 2u);
    uint32_t limit = p->target_mva < 5000 ? p->target_mva : 5000;
    if (r->apparent_mva > limit * 2u || r->voltage_mv > volts[p->range] * 70u / 100u ||
        r->current_ma > amps[p->range] * 70u / 100u) {
        power_fail(p, POWER_ERROR_LIMIT); return;
    }
    if (now < p->probe_after) return;
    if (r->voltage_mv >= min_mv && r->current_ma >= min_ma) {
        uint32_t impedance = (uint32_t)((uint64_t)r->voltage_mv * 1000u / r->current_ma);
        if (impedance < 500 || impedance > 2000000) { power_fail(p, POWER_ERROR_MATCH); return; }
        uint8_t range = (p->changes || p->rematches) ? stable_range(p->range, impedance) : closest(impedance);
        if (!consistent(p, range)) return;
        if (range != p->range) { switch_range(p, range); return; }
        p->matching = false;
        p->state = POWER_RUNNING;
        p->started = p->feedback = p->resident_since = p->low_since = now;
        p->ramp_base = p->amplitude;
        p->level_milli = (int64_t)p->amplitude * 1000;
        p->stable = 0;
        return;
    }
    bool bound = p->amplitude >= 511 || r->apparent_mva >= limit ||
        r->voltage_mv >= volts[p->range] * 35u / 100u || r->current_ma >= amps[p->range] * 35u / 100u;
    if (bound) {
        uint8_t next = p->range;
        /* 只有阻抗界也越过迟滞边界，才换挡再测；低电流本身不能判开路或最高挡。 */
        if (r->voltage_mv >= min_mv && r->current_ma < min_ma) {
            uint32_t lower = (uint32_t)((uint64_t)(r->voltage_mv - margin_mv) * 1000u /
                                       (r->current_ma + margin_ma));
            uint8_t candidate = stable_range(p->range, lower);
            if (candidate > next) next = candidate;
        } else if (r->current_ma >= min_ma && r->voltage_mv < min_mv) {
            uint32_t upper = (uint32_t)((uint64_t)(r->voltage_mv + margin_mv) * 1000u /
                                       (r->current_ma - margin_ma));
            uint8_t candidate = stable_range(p->range, upper);
            if (candidate < next) next = candidate;
        }
        if (next != p->range) {
            if (consistent(p, next)) switch_range(p, next);
        }
        else power_fail(p, POWER_ERROR_MATCH);
        return;
    }
    p->stable = 0;
    /* 最慢50ms窗口；留100ms等待空闲半区生效及一个完整稳定窗。 */
    uint16_t amplitude = p->amplitude + 32u;
    if (amplitude > 511) amplitude = 511;
    if (p->ops.level(p->ops.ctx, amplitude) != 0) { power_fail(p, POWER_ERROR_IO); return; }
    p->amplitude = amplitude;
    p->probe_after = p->ops.now(p->ops.ctx) + 100;
    (void)cancel(p);
}

void power_poll(struct power *p, bool permitted, const struct power_reading *r, int64_t now)
{
    if (p->state == POWER_IDLE || p->state == POWER_FAULT) return;
    if (cancel(p)) return;
    if (!permitted && p->state != POWER_STOPPING && p->state != POWER_RELEASE) {
        power_fail(p, POWER_ERROR_INTERLOCK);
        return;
    }
    if (p->automatic && p->matching && p->state != POWER_STOPPING && p->state != POWER_RELEASE && now >= p->match_deadline) {
        power_fail(p, POWER_ERROR_MATCH); return;
    }
    if (p->state == POWER_ZERO || p->state == POWER_PROBING || p->state == POWER_RUNNING) {
        if (!r || !r->valid || now < r->time_ms || now - r->time_ms > 150) {
            if (p->state != POWER_ZERO || now >= p->deadline) power_fail(p, POWER_ERROR_SAMPLE);
            return;
        }
        if (r->voltage_mv > volts[p->range] * 105u / 100u ||
            r->current_ma > amps[p->range] * 105u / 100u || r->apparent_mva > 52500) {
            power_fail(p, POWER_ERROR_LIMIT);
            return;
        }
        if (p->state == POWER_ZERO) {
            if (r->voltage_mv > 3000 || r->current_ma > 100) {
                power_fail(p, POWER_ERROR_SAMPLE);
                return;
            }
            p->state = p->automatic ? POWER_PROBING : POWER_RUNNING;
            p->noise_mv = r->voltage_mv;
            p->noise_ma = r->current_ma;
            p->probe_after = now;
            p->started = now;
            p->feedback = now;
            p->low_since = now;
            p->sequence = r->sequence;
            return;
        }
        if (p->sequence == r->sequence) return;
        p->sequence = r->sequence;
        if (p->state == POWER_PROBING) { probe(p, r, now); return; }
        if (p->automatic && now - p->resident_since >= 1000 &&
            r->voltage_mv >= maximum(800, p->noise_mv * 4u) && r->current_ma >= maximum(20, p->noise_ma * 4u)) {
            uint32_t impedance = (uint32_t)((uint64_t)r->voltage_mv * 1000u / r->current_ma);
            uint8_t next = stable_range(p->range, impedance);
            if (next != p->range && consistent(p, next)) {
                if (++p->rematches > 3) { power_fail(p, POWER_ERROR_MATCH); return; }
                p->changes = 0;
                p->match_deadline = now + 15000;
                switch_range(p, next);
                return;
            }
            if (next == p->range) p->stable = 0;
        } else p->stable = 0;
        int64_t dt = now - p->feedback;
        p->feedback = now;
        if (dt <= 0 || dt > 150) { power_fail(p, POWER_ERROR_SAMPLE); return; }
        /* 初始Ki=20码/(VA·s)，待实际负载整定；积分及软启动都按时间，不按调用次数。 */
        p->level_milli += ((int64_t)p->target_mva - r->apparent_mva) * dt / 50;
        int64_t ceiling = (int64_t)p->ramp_base * 1000 + (now - p->started) * 2047;
        if (ceiling > 2047000) ceiling = 2047000;
        if (p->level_milli < 0) p->level_milli = 0;
        if (p->level_milli > ceiling) p->level_milli = ceiling;
        p->amplitude = (uint16_t)(p->level_milli / 1000);
        if (r->current_ma >= 20 || p->amplitude < 1024) p->low_since = now;
        if (now - p->low_since > 1000) { power_fail(p, POWER_ERROR_OPEN); return; }
        if (r->current_ma > 200 && r->voltage_mv < 100 && now - p->started > 200) {
            power_fail(p, POWER_ERROR_SHORT);
            return;
        }
        if (p->ops.level(p->ops.ctx, p->amplitude) != 0) power_fail(p, POWER_ERROR_IO);
        else (void)cancel(p);
        return;
    }
    if (now < p->deadline) return;
    switch (p->state) {
    case POWER_MUTING:
        if (select_output(p, p->output & 1u)) p->state = POWER_BREAK;
        break;
    case POWER_BREAK:
        if (select_output(p, p->range >= 4 ? 1 : 0)) p->state = POWER_TRANS;
        break;
    case POWER_TRANS:
        if (select_output(p, (uint8_t)((p->range >= 4 ? 1u : 0u) | (1u << (p->range + 1u)))))
            p->state = POWER_TAP;
        break;
    case POWER_TAP:
        if (p->ops.start(p->ops.ctx, p->frequency) != 0) power_fail(p, POWER_ERROR_IO);
        else {
            p->state = POWER_ZERO;
            p->deadline = p->ops.now(p->ops.ctx) + 250;
            (void)cancel(p);
        }
        break;
    case POWER_STOPPING:
        if (select_output(p, p->output & 1u)) p->state = POWER_RELEASE;
        break;
    case POWER_RELEASE:
        if (select_output(p, 0)) p->state = POWER_IDLE;
        break;
    default: break;
    }
}
