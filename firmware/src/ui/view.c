/* @brief 格式化定点数并缓存页/动画同步状态，连续刷新保留屏端动画进度。 */
#include "view.h"

#include <string.h>

static bool valid_snapshot(const struct view_snapshot *snapshot)
{
    return (unsigned int)snapshot->page <= VIEW_PAGE_FAULT &&
           (unsigned int)snapshot->state <= VIEW_OFFLINE &&
           (unsigned int)snapshot->fault <= VIEW_FAULT_COMMUNICATION &&
           (unsigned int)snapshot->battery <= VIEW_POWER_UNKNOWN &&
           (unsigned int)snapshot->charger <= VIEW_POWER_UNKNOWN &&
           (unsigned int)snapshot->calibration <= VIEW_CALIBRATION_PENDING &&
           (unsigned int)snapshot->startup_step <= VIEW_STARTUP_ERROR &&
           (snapshot->mode == PANEL_CC || snapshot->mode == PANEL_VA) &&
           (unsigned int)snapshot->selected <= PANEL_RANGE;
}

static enum view_state display_state(const struct view_snapshot *snapshot)
{
    if (snapshot->page == VIEW_PAGE_FAULT || snapshot->state == VIEW_FAULT ||
        (snapshot->page == VIEW_PAGE_STARTUP && snapshot->startup_step == VIEW_STARTUP_ERROR)) {
        return VIEW_FAULT;
    }
    if (snapshot->state == VIEW_OFFLINE ||
        (snapshot->page == VIEW_PAGE_MEASURE && !snapshot->fresh)) {
        return VIEW_OFFLINE;
    }
    return snapshot->page == VIEW_PAGE_STARTUP ? VIEW_STANDBY : snapshot->state;
}

static int send_words(enum dgus_crc crc, view_send_fn send, void *ctx,
                       uint16_t vp, const uint16_t *words, size_t count)
{
    uint8_t frame[DGUS_MAX_FRAME];
    int length = dgus_encode_write(crc, vp, words, count, frame, sizeof(frame));

    if (length < 0) {
        return VIEW_ERR_ENCODING;
    }
    return send(ctx, frame, (size_t)length) == 0 ? VIEW_OK : VIEW_ERR_SEND;
}

static void format_value(char text[VIEW_TEXT_BYTES], int64_t value,
                          unsigned int decimals, unsigned int max_chars, bool valid)
{
    char digits[20];
    uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    uint32_t scale = decimals == 3 ? 1000u : decimals == 1 ? 10u : 1u;
    uint64_t integer = magnitude / scale;
    uint32_t fraction = (uint32_t)(magnitude % scale);
    size_t count = 0;
    size_t used = 0;

    memset(text, 0, VIEW_TEXT_BYTES);
    do {
        digits[count++] = (char)('0' + integer % 10u);
        integer /= 10u;
    } while (integer != 0);
    if (!valid || count + (value < 0 ? 1u : 0u) +
                  (decimals > 0 ? decimals + 1u : 0u) > max_chars) {
        text[0] = '-';
        text[1] = '-';
        return;
    }
    if (value < 0) {
        text[used++] = '-';
    }
    while (count != 0) {
        text[used++] = digits[--count];
    }
    if (decimals > 0) {
        text[used++] = '.';
        for (uint32_t divisor = scale / 10u; divisor != 0; divisor /= 10u) {
            text[used++] = (char)('0' + fraction / divisor);
            fraction %= divisor;
        }
    }
}

static int send_values(const struct view_snapshot *snapshot, enum view_state state, enum dgus_crc crc,
                        view_send_fn send, void *ctx)
{
    static const unsigned int decimals[VIEW_FIELD_COUNT] = {
        3, 3, 3, 1, 3, 3, 3, 3, 0, 3, 3, 1, 1, 1, 3, 0
    };
    static const unsigned int max_chars[VIEW_FIELD_COUNT] = {
        9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 7, 7, 7, 8, 9
    };
    bool readings_valid = snapshot->fresh && state != VIEW_FAULT &&
                          state != VIEW_OFFLINE && state != VIEW_SWITCHING;

    for (size_t field = 0; field < VIEW_FIELD_COUNT; ++field) {
        char text[VIEW_TEXT_BYTES];
        uint16_t words[VIEW_TEXT_BYTES / 2];
        bool setting = field == VIEW_TARGET || field == VIEW_RANGE || field == VIEW_REQUEST_FREQUENCY;
        bool valid = snapshot->values[field].valid && (setting || readings_valid);
        int result;

        format_value(text, snapshot->values[field].value, decimals[field], max_chars[field], valid);
        for (size_t i = 0; i < VIEW_TEXT_BYTES / 2; ++i) {
            words[i] = (uint16_t)(((uint16_t)(uint8_t)text[i * 2] << 8) |
                                  (uint8_t)text[i * 2 + 1]);
        }
        result = send_words(crc, send, ctx, (uint16_t)(0x1100u + field * 0x10u),
                            words, VIEW_TEXT_BYTES / 2);
        if (result != VIEW_OK) {
            return result;
        }
    }
    return VIEW_OK;
}

void view_reset(struct view *view)
{
    if (view != NULL) {
        *view = (struct view){0};
    }
}

int view_refresh(struct view *view, const struct view_snapshot *snapshot, enum dgus_crc crc,
                 view_send_fn send, void *ctx)
{
    const uint16_t stop = 0;
    const uint16_t start = 1;
    uint16_t icons[9];
    uint16_t page[2];
    enum view_state state;
    bool run_animation;
    bool startup_animation;
    bool page_changed;
    bool transition;
    int result;

    if (view == NULL || snapshot == NULL || send == NULL || !valid_snapshot(snapshot)) {
        return VIEW_ERR_ARG;
    }
    if (crc != DGUS_CRC_NONE && crc != DGUS_CRC_MODBUS) {
        return VIEW_ERR_CRC;
    }
    state = display_state(snapshot);
    run_animation = snapshot->page == VIEW_PAGE_MEASURE && state == VIEW_RUNNING;
    startup_animation = snapshot->page == VIEW_PAGE_STARTUP &&
                        state != VIEW_FAULT && state != VIEW_OFFLINE &&
                        snapshot->startup_step != VIEW_STARTUP_READY;
    page_changed = !view->synced || view->page != snapshot->page;
    transition = page_changed || view->state != state ||
                 view->run_animation != run_animation || view->startup_animation != startup_animation;
    if (transition) {
        result = send_words(crc, send, ctx, 0x1000, &stop, 1);
        if (result != VIEW_OK) {
            goto failed;
        }
        result = send_words(crc, send, ctx, 0x1002, &stop, 1);
        if (result != VIEW_OK) {
            goto failed;
        }
    }
    icons[0] = (uint16_t)snapshot->selected;
    icons[1] = (uint16_t)snapshot->mode;
    icons[2] = snapshot->auto_range ? 1u : 0u;
    icons[3] = (uint16_t)state;
    icons[4] = (uint16_t)snapshot->fault;
    icons[5] = (uint16_t)snapshot->battery;
    icons[6] = (uint16_t)snapshot->charger;
    icons[7] = (uint16_t)snapshot->calibration;
    icons[8] = (uint16_t)snapshot->startup_step;
    result = send_words(crc, send, ctx, 0x1004, icons, 9);
    if (result != VIEW_OK) {
        goto failed;
    }
    result = send_values(snapshot, state, crc, send, ctx);
    if (result != VIEW_OK) {
        goto failed;
    }
    if (page_changed) {
        page[0] = 0x5a01;
        page[1] = (uint16_t)snapshot->page;
        result = send_words(crc, send, ctx, 0x0084, page, 2);
        if (result != VIEW_OK) {
            goto failed;
        }
    }
    if (transition && (run_animation || startup_animation)) {
        result = send_words(crc, send, ctx, run_animation ? 0x1000 : 0x1002, &start, 1);
        if (result != VIEW_OK) {
            goto failed;
        }
    }
    *view = (struct view){
        .synced = true,
        .page = snapshot->page,
        .state = state,
        .run_animation = run_animation,
        .startup_animation = startup_animation
    };
    return VIEW_OK;

failed:
    view_reset(view);
    return result;
}
