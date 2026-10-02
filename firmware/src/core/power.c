/* @brief 静音后先断后合，按真实VA反馈积分调幅；停止/故障取消后续合闸。 */
#include "core/power.h"
#include <stddef.h>

static const uint32_t volts[] = {7070, 12250, 22360, 38730, 70700, 122500, 223600};
static const uint32_t amps[] = {7070, 4080, 2240, 1290, 710, 408, 224};

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
    if (p->state != POWER_IDLE || range >= 7 || target == 0 || target > 50000 ||
        (frequency != 2000 && frequency != 5000 && frequency != 8000 && frequency != 10000)) return -1;
    if (p->ops.cancelled && p->ops.cancelled(p->ops.ctx)) return -1;
    p->range = range;
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

void power_poll(struct power *p, bool permitted, const struct power_reading *r, int64_t now)
{
    if (p->state == POWER_IDLE || p->state == POWER_FAULT) return;
    if (cancel(p)) return;
    if (!permitted && p->state != POWER_STOPPING && p->state != POWER_RELEASE) {
        power_fail(p, POWER_ERROR_INTERLOCK);
        return;
    }
    if (p->state == POWER_ZERO || p->state == POWER_RUNNING) {
        if (!r || !r->valid || now < r->time_ms || now - r->time_ms > 150) {
            if (p->state == POWER_RUNNING || now >= p->deadline) power_fail(p, POWER_ERROR_SAMPLE);
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
            p->state = POWER_RUNNING;
            p->started = now;
            p->feedback = now;
            p->low_since = now;
            p->sequence = r->sequence;
            return;
        }
        if (p->sequence == r->sequence) return;
        p->sequence = r->sequence;
        int64_t dt = now - p->feedback;
        p->feedback = now;
        if (dt <= 0 || dt > 150) { power_fail(p, POWER_ERROR_SAMPLE); return; }
        /* 初始Ki=20码/(VA·s)，待实际负载整定；积分及软启动都按时间，不按调用次数。 */
        p->level_milli += ((int64_t)p->target_mva - r->apparent_mva) * dt / 50;
        int64_t ceiling = (now - p->started) * 2047;
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
