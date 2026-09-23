/* @brief 格式化已生效状态与独立候选值，同步两页屏幕契约。 */
#include "view.h"
#include <string.h>

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

static void format_number(char *text, int64_t value, unsigned int decimals)
{
    char digits[20];
    uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    uint32_t scale = decimals == 3 ? 1000u : 1u;
    uint64_t integer = magnitude / scale;
    uint32_t fraction = (uint32_t)(magnitude % scale);
    size_t count = 0;
    size_t used = 0;
    do {
        digits[count++] = (char)('0' + integer % 10u);
        integer /= 10u;
    } while (integer != 0);
    if (count + (value < 0 ? 1u : 0u) + (decimals ? 4u : 0u) > VIEW_TEXT_MAX_CHARS) {
        return;
    }
    if (value < 0) {
        text[used++] = '-';
    }
    while (count != 0) {
        text[used++] = digits[--count];
    }
    if (decimals) {
        text[used++] = '.';
        for (uint32_t divisor = 100u; divisor != 0; divisor /= 10u) {
            text[used++] = (char)('0' + fraction / divisor);
            fraction %= divisor;
        }
    }
    text[used] = '\0';
}

static void format_time(char *text, int64_t seconds)
{
    uint32_t hours;
    uint32_t minutes;
    uint32_t remainder;
    size_t used = 0;
    if (seconds < 0 || seconds > 3599999) {
        return;
    }
    hours = (uint32_t)seconds / 3600u;
    minutes = (uint32_t)seconds / 60u % 60u;
    remainder = (uint32_t)seconds % 60u;
    if (hours >= 100u) {
        text[used++] = (char)('0' + hours / 100u);
    }
    text[used++] = (char)('0' + hours / 10u % 10u);
    text[used++] = (char)('0' + hours % 10u);
    text[used++] = ':';
    text[used++] = (char)('0' + minutes / 10u);
    text[used++] = (char)('0' + minutes % 10u);
    text[used++] = ':';
    text[used++] = (char)('0' + remainder / 10u);
    text[used++] = (char)('0' + remainder % 10u);
    text[used] = '\0';
}

static void format_value(char text[VIEW_TEXT_BYTES], enum view_field field, int64_t value, bool valid)
{
    memset(text, 0, VIEW_TEXT_BYTES);
    text[0] = '-';
    text[1] = '-';
    if (!valid) {
        return;
    }
    if (field == VIEW_ELAPSED) {
        format_time(text, value);
    } else if (field == VIEW_FREQUENCY || field == VIEW_FREQUENCY_CHOICE) {
        if (value == 2000 || value == 5000 || value == 8000 || value == 10000) {
            format_number(text, value / 1000, 0);
        }
    } else if (field == VIEW_RANGE || field == VIEW_RANGE_CHOICE) {
        if (value == 1 || value == 3 || value == 10 || value == 30 ||
            value == 100 || value == 300 || value == 1000) {
            format_number(text, value, 0);
        }
    } else {
        format_number(text, value, 3);
    }
}

static int send_values(const struct view_snapshot *snapshot, enum dgus_crc crc,
                        view_send_fn send, void *ctx)
{
    for (size_t field = 0; field < VIEW_FIELD_COUNT; ++field) {
        char text[VIEW_TEXT_BYTES];
        uint16_t words[VIEW_TEXT_BYTES / 2];
        bool setting = field == VIEW_RANGE_CHOICE || field == VIEW_FREQUENCY_CHOICE;
        bool valid = snapshot->values[field].valid &&
                     (setting || (snapshot->fresh && snapshot->state != VIEW_OFFLINE));
        int result;
        if (field <= VIEW_VOLTAGE &&
            (snapshot->state == VIEW_FAULT || snapshot->state == VIEW_SWITCHING)) {
            valid = false;
        }
        format_value(text, (enum view_field)field, snapshot->values[field].value, valid);
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
    uint16_t icons[4];
    uint16_t page[2];
    bool online;
    int result;
    if (view == NULL || snapshot == NULL || send == NULL ||
        (unsigned int)snapshot->page > PANEL_PAGE_SETTINGS ||
        (unsigned int)snapshot->state > VIEW_OFFLINE ||
        (unsigned int)snapshot->battery > VIEW_POWER_UNKNOWN ||
        (unsigned int)snapshot->selected > PANEL_FREQUENCY) {
        return VIEW_ERR_ARG;
    }
    if (crc != DGUS_CRC_NONE && crc != DGUS_CRC_MODBUS) {
        return VIEW_ERR_CRC;
    }
    online = snapshot->fresh && snapshot->state != VIEW_OFFLINE;
    icons[0] = (uint16_t)(snapshot->state == VIEW_FAULT ? VIEW_FAULT :
                         online ? snapshot->state : VIEW_OFFLINE);
    icons[1] = (uint16_t)(online ? snapshot->battery : VIEW_POWER_UNKNOWN);
    icons[2] = (uint16_t)snapshot->selected;
    icons[3] = snapshot->editing ? 1u : 0u;
    result = send_words(crc, send, ctx, 0x1000, icons, 4);
    if (result == VIEW_OK) {
        result = send_values(snapshot, crc, send, ctx);
    }
    if (result == VIEW_OK && (!view->synced || view->page != snapshot->page)) {
        page[0] = 0x5a01;
        page[1] = (uint16_t)snapshot->page;
        result = send_words(crc, send, ctx, 0x0084, page, 2);
    }
    if (result != VIEW_OK) {
        view_reset(view);
        return result;
    }
    *view = (struct view){.synced = true, .page = snapshot->page};
    return VIEW_OK;
}
