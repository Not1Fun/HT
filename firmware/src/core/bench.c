/* @brief 静音后先断抽头再切变压器；试波10秒、继电器30秒后自动释放。 */
#include "bench.h"
#include "config/analog.h"

uint8_t bench_mask(uint8_t relay)
{
    if (relay < 2) return relay;
    return relay <= 8 ? (uint8_t)((1u << (relay - 1u)) | (relay >= 6 ? 1u : 0u)) : 0;
}

int bench_init(struct bench *b, const struct power_ops *ops)
{
    if (!b || !ops || !ops->mute || !ops->select || !ops->start || !ops->level || !ops->now) return -1;
    *b = (struct bench){.ops = *ops};
    return 0;
}

void bench_fail(struct bench *b, enum power_error error)
{
    int quiet = b->ops.mute(b->ops.ctx);
    int off = b->ops.select(b->ops.ctx, 0);
    if (off == 0) b->output = 0;
    b->fault_stopped = quiet == 0 && off == 0;
    if (b->state != BENCH_FAULT) b->error = error;
    b->state = BENCH_FAULT;
}

static bool cancelled(struct bench *b)
{
    return b->ops.cancelled && b->ops.cancelled(b->ops.ctx);
}

static bool select_output(struct bench *b, uint8_t value)
{
    if (b->ops.select(b->ops.ctx, value) != 0) {
        bench_fail(b, POWER_ERROR_IO);
        return false;
    }
    b->output = value;
    b->deadline = b->ops.now(b->ops.ctx) + 30;
    return true;
}

void bench_stop(struct bench *b, int64_t now)
{
    if (b->state == BENCH_IDLE || b->state == BENCH_FAULT ||
        b->state == BENCH_STOP || b->state == BENCH_RELEASE) return;
    if (b->ops.mute(b->ops.ctx) != 0) { bench_fail(b, POWER_ERROR_IO); return; }
    b->state = BENCH_STOP;
    b->deadline = b->ops.now(b->ops.ctx) + 30;
    (void)now;
}

int bench_start(struct bench *b, uint8_t relay, uint32_t frequency,
                uint16_t millivolts_pp, bool wave, int64_t now)
{
    if (relay > 8 || (frequency != 2000 && frequency != 5000 && frequency != 8000 && frequency != 10000) ||
        (millivolts_pp != 10 && millivolts_pp != 25 && millivolts_pp != 50 && millivolts_pp != 100) ||
        b->state == BENCH_FAULT || b->state == BENCH_STOP || b->state == BENCH_RELEASE || cancelled(b)) return -1;
    if (b->ops.mute(b->ops.ctx) != 0) { bench_fail(b, POWER_ERROR_IO); return -1; }
    b->relay = relay;
    b->frequency = frequency;
    b->wave = wave;
    b->amplitude = (uint16_t)((uint32_t)millivolts_pp * 4095u / (2u * HT_VREF_MV));
    b->state = BENCH_MUTE;
    b->deadline = b->ops.now(b->ops.ctx) + 30;
    b->expires = now + (wave ? 10000 : 30000);
    return 0;
}

void bench_poll(struct bench *b, bool permitted, const struct power_reading *r, int64_t now)
{
    if (b->state == BENCH_IDLE || b->state == BENCH_FAULT) return;
    if (cancelled(b) || !permitted || now >= b->expires) bench_stop(b, now);
    if (b->state == BENCH_STOP || b->state == BENCH_RELEASE) {
        if (now < b->deadline) return;
        if (b->state == BENCH_STOP) {
            if (select_output(b, b->output & 1u)) b->state = BENCH_RELEASE;
        } else if (select_output(b, 0)) b->state = BENCH_IDLE;
        return;
    }
    bool fresh = r && r->valid && now >= r->time_ms && now - r->time_ms < 150;
    /* 固定幅度无自动增益；仍保留测量、新鲜度和低功率边界。 */
    if (fresh && (r->voltage_mv > 30000 || r->current_ma > 1500 || r->apparent_mva > 1000)) {
        bench_fail(b, POWER_ERROR_LIMIT); return;
    }
    if (b->state == BENCH_ON && !fresh) { bench_fail(b, POWER_ERROR_SAMPLE); return; }
    if (now < b->deadline) return;
    uint8_t mask = bench_mask(b->relay);
    switch (b->state) {
    case BENCH_MUTE:
        if (select_output(b, b->output & 1u)) b->state = BENCH_BREAK;
        break;
    case BENCH_BREAK:
        if (select_output(b, mask & 1u)) b->state = BENCH_TRANS;
        break;
    case BENCH_TRANS:
        if (select_output(b, mask)) b->state = BENCH_TAP;
        break;
    case BENCH_TAP:
        if (!b->wave) { b->state = BENCH_ON; break; }
        if (b->ops.start(b->ops.ctx, b->frequency) != 0) { bench_fail(b, POWER_ERROR_IO); break; }
        b->state = BENCH_ZERO;
        b->deadline = b->ops.now(b->ops.ctx) + 250;
        break;
    case BENCH_ZERO:
        if (!fresh) { bench_fail(b, POWER_ERROR_SAMPLE); break; }
        if (r->voltage_mv > 3000 || r->current_ma > 100) { bench_fail(b, POWER_ERROR_LIMIT); break; }
        if (cancelled(b)) { bench_stop(b, now); break; }
        if (b->ops.level(b->ops.ctx, b->amplitude) != 0) { bench_fail(b, POWER_ERROR_IO); break; }
        b->state = BENCH_ON;
        break;
    default: break;
    }
    if (cancelled(b)) bench_stop(b, now);
}
