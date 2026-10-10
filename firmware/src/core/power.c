/* @brief 稳定阻抗选挡与恒VA调幅；继电器先断后合，停止/故障优先。 */
#include "core/power.h"
#include "config/analog.h"
#include "config/output.h"
#include <stddef.h>

_Static_assert(HT_AMP_GAIN_MILLI > 0 && HT_PRIMARY_RATED_RMS_MV > 0 &&
               HT_PRIMARY_MAX_RMS_MV > 0, "Output calibration must be positive");

static const uint32_t volts[] = {7070, 12250, 22360, 38730, 70700, 122500, 223600};
static const uint32_t amps[] = {7070, 4080, 2240, 1290, 710, 408, 224};
static const uint32_t boundaries[] = {1732, 5477, 17321, 54772, 173205, 547723}; /* mΩ */
static const uint32_t loads[] = {1000, 3000, 10000, 30000, 100000, 300000, 1000000};

static uint32_t root(uint64_t value)
{
    uint64_t result = 0, bit = UINT64_C(1) << 62;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= result + bit) { value -= result + bit; result = (result >> 1) + bit; }
        else result >>= 1;
        bit >>= 2;
    }
    return (uint32_t)result;
}

static uint16_t primary_amplitude(uint32_t rms_mv)
{
    uint64_t code = (uint64_t)rms_mv * 4095u * 1414214u /
        ((uint64_t)HT_VREF_MV * HT_AMP_GAIN_MILLI * 1000u);
    return (uint16_t)(code > 2047 ? 2047 : code);
}

uint16_t power_drive_limit(void) { return primary_amplitude(HT_PRIMARY_MAX_RMS_MV); }

uint16_t power_feedforward(uint8_t range, uint32_t target, uint32_t load)
{
    if (range >= 7 || !load || !target || target > 50000) return 0;
    uint32_t rms = root((uint64_t)target * load);
    uint64_t primary = (uint64_t)rms * HT_PRIMARY_RATED_RMS_MV / volts[range];
    if (primary > HT_PRIMARY_MAX_RMS_MV) primary = HT_PRIMARY_MAX_RMS_MV;
    return primary_amplitude((uint32_t)primary);
}

static uint32_t capacity(uint8_t range, uint32_t load)
{
    uint64_t voltage = (uint64_t)volts[range] * volts[range] / load;
    uint64_t current = (uint64_t)amps[range] * amps[range] * load / 1000000u;
    uint64_t result = voltage < current ? voltage : current;
    return result > 50000 ? 50000 : (uint32_t)result;
}

static bool attainable(uint8_t range, uint32_t load, uint32_t target)
{
    uint32_t limit = capacity(range, load);
    /* 规格表电流取整；保留2%数值余量，运行硬限值不变。 */
    return target <= limit + limit / 50u;
}

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

static uint8_t choose_range(uint8_t current, uint32_t load, uint32_t target, bool keep)
{
    uint8_t range = keep && attainable(current, load, target) ?
        stable_range(current, load) : closest(load);
    if (!attainable(range, load, target)) {
        for (uint8_t i = 0; i < 7; ++i)
            if (capacity(i, load) > capacity(range, load)) range = i;
    }
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
    p->changes = p->rematches = p->stable = p->overload = 0;
    p->ramp_base = 0;
    p->load_mohm = p->automatic ? 0 : loads[range];
    p->limit_since = 0;
    p->impedance_count = p->impedance_next = 0;
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
    p->amplitude = p->ramp_base = p->stable = p->overload = 0;
    p->level_milli = 0;
    p->matching = true;
    p->impedance_count = p->impedance_next = 0;
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

static uint32_t tolerance(uint32_t target) { return maximum(10, target / 50u); }

static bool measure_load(struct power *p, const struct power_reading *r, uint32_t *load)
{
    if (r->voltage_mv < maximum(500, p->noise_mv * 3u) ||
        r->current_ma < maximum(20, p->noise_ma * 3u)) {
        p->impedance_count = 0;
        return false;
    }
    p->impedance[p->impedance_next] = (uint32_t)((uint64_t)r->voltage_mv * 1000u / r->current_ma);
    p->impedance_next = (p->impedance_next + 1u) % 3u;
    if (p->impedance_count < 3) ++p->impedance_count;
    if (p->impedance_count < 3) return false;
    uint32_t sorted[3] = {p->impedance[0], p->impedance[1], p->impedance[2]};
    for (size_t i = 1; i < 3; ++i)
        for (size_t j = i; j > 0 && sorted[j] < sorted[j-1]; --j) {
            uint32_t value = sorted[j]; sorted[j] = sorted[j-1]; sorted[j-1] = value;
        }
    if (sorted[2] - sorted[0] > sorted[1] / 5u) return false;
    *load = sorted[1];
    return true;
}

static void probe(struct power *p, const struct power_reading *r, int64_t now)
{
    uint32_t min_mv = maximum(500, p->noise_mv * 3u);
    uint32_t min_ma = maximum(20, p->noise_ma * 3u);
    uint32_t margin_mv = maximum(200, p->noise_mv * 2u);
    uint32_t margin_ma = maximum(5, p->noise_ma * 2u);
    uint32_t limit = p->target_mva < 5000 ? p->target_mva : 5000;
    if (r->apparent_mva > limit * 2u || r->voltage_mv > volts[p->range] * 70u / 100u ||
        r->current_ma > amps[p->range] * 70u / 100u) {
        power_fail(p, POWER_ERROR_LIMIT); return;
    }
    if (r->time_ms < p->probe_after) return;
    if (r->voltage_mv >= min_mv && r->current_ma >= min_ma) {
        uint32_t impedance;
        if (!measure_load(p, r, &impedance)) { p->stable = 0; return; }
        if (impedance < 500 || impedance > 2000000) { power_fail(p, POWER_ERROR_MATCH); return; }
        uint8_t range = choose_range(p->range, impedance, p->target_mva, p->changes || p->rematches);
        if (!consistent(p, range)) return;
        p->load_mohm = impedance;
        if (range != p->range) { switch_range(p, range); return; }
        p->matching = false;
        p->state = POWER_RUNNING;
        p->started = p->feedback = p->resident_since = p->low_since = now;
        p->ramp_base = p->amplitude;
        p->level_milli = (int64_t)p->amplitude * 1000;
        p->stable = 0;
        return;
    }
    p->impedance_count = 0;
    uint16_t probe_limit = (uint16_t)(power_drive_limit() * 35u / 100u);
    bool bound = p->amplitude >= probe_limit || r->apparent_mva >= limit ||
        r->voltage_mv >= volts[p->range] * 35u / 100u || r->current_ma >= amps[p->range] * 35u / 100u;
    if (bound) {
        uint8_t next = p->range;
        /* 只有阻抗界也越过迟滞边界，才换挡再测；低电流本身不能判开路或最高挡。 */
        if (r->voltage_mv >= min_mv && r->current_ma < min_ma) {
            uint32_t lower = (uint32_t)((uint64_t)(r->voltage_mv - margin_mv) * 1000u /
                                       (r->current_ma + margin_ma));
            uint8_t candidate = stable_range(p->range, lower);
            if (candidate > next) { next = candidate; p->load_mohm = lower; }
        } else if (r->current_ma >= min_ma && r->voltage_mv < min_mv) {
            uint32_t upper = (uint32_t)((uint64_t)(r->voltage_mv + margin_mv) * 1000u /
                                       (r->current_ma - margin_ma));
            uint8_t candidate = stable_range(p->range, upper);
            if (candidate < next) { next = candidate; p->load_mohm = upper; }
        }
        if (next != p->range) {
            if (consistent(p, next)) switch_range(p, next);
        }
        else power_fail(p, r->voltage_mv >= min_mv && p->range == 6 ? POWER_ERROR_OPEN : POWER_ERROR_FEEDBACK);
        return;
    }
    p->stable = 0;
    if (r->voltage_mv >= min_mv && r->current_ma < min_ma)
        p->load_mohm = (uint32_t)((uint64_t)(r->voltage_mv - margin_mv) * 1000u /
            (r->current_ma + margin_ma));
    /* 最慢50ms窗口；留100ms等待空闲半区生效及一个完整稳定窗。 */
    uint16_t amplitude = p->amplitude + 32u;
    if (amplitude > probe_limit) amplitude = probe_limit;
    if (p->load_mohm) {
        uint16_t ceiling = power_feedforward(p->range, limit, p->load_mohm);
        if (ceiling && amplitude > ceiling) amplitude = ceiling;
    }
    if (amplitude <= p->amplitude) { power_fail(p, POWER_ERROR_FEEDBACK); return; }
    if (p->ops.level(p->ops.ctx, amplitude) != 0) { power_fail(p, POWER_ERROR_IO); return; }
    p->amplitude = amplitude;
    p->probe_after = p->ops.now(p->ops.ctx) + 100;
    (void)cancel(p);
}

static void regulate(struct power *p, const struct power_reading *r, bool load_valid, int64_t now)
{
    uint16_t maximum_code = power_drive_limit();
    uint32_t min_mv = maximum(500, p->noise_mv * 3u);
    uint32_t min_ma = maximum(20, p->noise_ma * 3u);
    bool useful = r->voltage_mv >= min_mv && r->current_ma >= min_ma;
    if (r->current_ma > 200 && r->voltage_mv < maximum(100, p->noise_mv * 2u) && now - p->started > 200) {
        power_fail(p, POWER_ERROR_SHORT); return;
    }
    uint16_t feedforward = power_feedforward(p->range, p->target_mva, p->load_mohm);
    if (useful || p->amplitude < maximum(16, feedforward / 4u)) p->low_since = now;
    if (now - p->low_since > 2000) {
        power_fail(p, r->voltage_mv >= min_mv && r->current_ma < min_ma ? POWER_ERROR_OPEN : POWER_ERROR_FEEDBACK);
        return;
    }
    if (load_valid && r->apparent_mva >= p->target_mva / 4u &&
        !attainable(p->range, p->load_mohm, p->target_mva)) {
        if (++p->overload >= 3) power_fail(p, POWER_ERROR_TARGET);
        return;
    } else if (load_valid) p->overload = 0;
    if (p->amplitude + 2u < maximum_code || r->apparent_mva + tolerance(p->target_mva) >= p->target_mva)
        p->limit_since = now;
    if (now - p->limit_since > 2000) { power_fail(p, POWER_ERROR_TARGET); return; }
    if (r->time_ms < p->probe_after) return;
    int64_t dt = now - p->feedback;
    p->feedback = now;
    if (dt <= 0) return;
    if (dt > 150) dt = 150;
    int64_t desired = (int64_t)feedforward * 1000;
    if (useful && p->amplitude && r->apparent_mva) {
        desired = root((uint64_t)p->target_mva * p->amplitude * p->amplitude * 1000000u / r->apparent_mva);
    }
    uint32_t error = r->apparent_mva > p->target_mva ? r->apparent_mva - p->target_mva : p->target_mva - r->apparent_mva;
    if (useful && error <= tolerance(p->target_mva)) desired = p->level_milli;
    int64_t next = p->level_milli + (desired - p->level_milli) * dt / (dt + 150);
    int64_t step = (int64_t)maximum_code * dt;
    if (next > p->level_milli + step) next = p->level_milli + step;
    if (next < p->level_milli - step) next = p->level_milli - step;
    int64_t ceiling = (int64_t)p->ramp_base * 1000 + (now - p->started) * maximum_code;
    if (ceiling > (int64_t)maximum_code * 1000) ceiling = (int64_t)maximum_code * 1000;
    if (next < 0) next = 0;
    if (next > ceiling) next = ceiling;
    uint16_t amplitude = (uint16_t)(next / 1000);
    p->level_milli = next;
    if (amplitude != p->amplitude) {
        if (p->ops.level(p->ops.ctx, amplitude) != 0) { power_fail(p, POWER_ERROR_IO); return; }
        p->amplitude = amplitude;
        p->probe_after = p->ops.now(p->ops.ctx) + 100;
        (void)cancel(p);
    }
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
        if (!r || !r->valid || now < r->time_ms || now - r->time_ms > 150 ||
            (p->state == POWER_ZERO && r->time_ms <= p->probe_after)) {
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
            p->limit_since = now;
            p->sequence = r->sequence;
            return;
        }
        if (p->sequence == r->sequence) return;
        p->sequence = r->sequence;
        if (p->state == POWER_PROBING) { probe(p, r, now); return; }
        bool settled = r->time_ms >= p->probe_after;
        uint32_t impedance;
        bool load_valid = settled && measure_load(p, r, &impedance);
        if (load_valid) p->load_mohm = impedance;
        if (p->automatic && load_valid && (now - p->resident_since >= 1000 ||
            !attainable(p->range, impedance, p->target_mva))) {
            uint8_t next = choose_range(p->range, impedance, p->target_mva, true);
            if (next != p->range && consistent(p, next)) {
                if (++p->rematches > 3) { power_fail(p, POWER_ERROR_MATCH); return; }
                p->changes = 0;
                p->match_deadline = now + 15000;
                switch_range(p, next);
                return;
            }
            if (next == p->range) p->stable = 0;
            else return; /* 待选挡确认时保持幅度，避免在容量不足的旧挡上继续升幅。 */
        } else if (settled) p->stable = 0;
        regulate(p, r, load_valid, now);
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
            p->probe_after = p->ops.now(p->ops.ctx);
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
