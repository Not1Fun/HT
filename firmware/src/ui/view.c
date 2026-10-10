/* @brief 格式化状态、独立候选值与事件记录，同步屏幕契约。 */
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
    uint32_t scale = decimals == 3 ? 1000u : decimals == 1 ? 10u : 1u;
    uint64_t integer = magnitude / scale;
    uint32_t fraction = (uint32_t)(magnitude % scale);
    size_t count = 0;
    size_t used = 0;
    do {
        digits[count++] = (char)('0' + integer % 10u);
        integer /= 10u;
    } while (integer != 0);
    if (count + (value < 0 ? 1u : 0u) + (decimals ? decimals + 1u : 0u) > VIEW_TEXT_MAX_CHARS) {
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
        for (uint32_t divisor = scale / 10u; divisor != 0; divisor /= 10u) {
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
    } else if (field == VIEW_POWER_CHOICE) {
        if (value >= 0 && value <= 50000) format_number(text, value / 1000, 0);
    } else if (field == VIEW_FREQUENCY || field == VIEW_FREQUENCY_CHOICE) {
        if (value == 2000 || value == 5000 || value == 8000 || value == 10000) {
            format_number(text, value / 1000, 0);
        }
    } else if (field == VIEW_RANGE || field == VIEW_RANGE_CHOICE) {
        if (field == VIEW_RANGE_CHOICE && value == 0) strcpy(text, "AUTO");
        if (value == 1 || value == 3 || value == 10 || value == 30 ||
            value == 100 || value == 300 || value == 1000) {
            format_number(text, value, 0);
        }
    } else if (field >= VIEW_NTC1 && field <= VIEW_NTC3) {
        if (value >= -200 && value <= 1200) {
            format_number(text, value, 1);
        }
    } else {
        format_number(text, value, 3);
    }
}

static int send_text(enum dgus_crc crc, view_send_fn send, void *ctx,
                     uint16_t vp, const char text[VIEW_TEXT_BYTES])
{
    uint16_t words[VIEW_TEXT_BYTES / 2];
    for (size_t i = 0; i < VIEW_TEXT_BYTES / 2; ++i) {
        words[i] = (uint16_t)(((uint16_t)(uint8_t)text[i * 2] << 8) |
                              (uint8_t)text[i * 2 + 1]);
    }
    return send_words(crc, send, ctx, vp, words, VIEW_TEXT_BYTES / 2);
}

static int send_values(const struct view_snapshot *snapshot, enum dgus_crc crc,
                        view_send_fn send, void *ctx)
{
    static const uint16_t addresses[VIEW_FIELD_COUNT] = {
        [VIEW_CURRENT] = 0x1100, [VIEW_VOLTAGE] = 0x1110, [VIEW_RANGE] = 0x1120,
        [VIEW_FREQUENCY] = 0x1130, [VIEW_ELAPSED] = 0x1140,
        [VIEW_FREQUENCY_CHOICE] = 0x1160, [VIEW_NTC1] = 0x1170,
        [VIEW_NTC2] = 0x1180, [VIEW_NTC3] = 0x1190, [VIEW_POWER_CHOICE] = 0x11a0,
        [VIEW_RANGE_CHOICE] = 0x1150
    };
    for (size_t field = 0; field < VIEW_FIELD_COUNT; ++field) {
        char text[VIEW_TEXT_BYTES];
        bool setting = field == VIEW_FREQUENCY_CHOICE || field == VIEW_POWER_CHOICE || field == VIEW_RANGE_CHOICE;
        bool valid = snapshot->values[field].valid &&
                     (setting || (snapshot->fresh && snapshot->state != VIEW_OFFLINE));
        int result;
        if (field <= VIEW_VOLTAGE && snapshot->state == VIEW_FAULT) {
            valid = false;
        }
        format_value(text, (enum view_field)field, snapshot->values[field].value, valid);
        result = send_text(crc, send, ctx, addresses[field], text);
        if (result != VIEW_OK) {
            return result;
        }
    }
    return VIEW_OK;
}

static void format_log_value(char text[VIEW_TEXT_BYTES], const struct event_entry *entry)
{
    memset(text, 0, VIEW_TEXT_BYTES);
    switch (entry->kind) {
    case EVENT_BOOT:
        if (entry->value == EVENT_BOOT_BENCH) {
            memcpy(text, "BENCH", 5);
        }
        break;
    case EVENT_RANGE:
        format_value(text, VIEW_RANGE, entry->value, true);
        if (text[0] != '-') {
            strcat(text, " ohm");
        }
        break;
    case EVENT_FREQUENCY:
    case EVENT_DDS_START:
        format_value(text, VIEW_FREQUENCY, entry->value, true);
        if (text[0] != '-') {
            strcat(text, " kHz");
        }
        break;
    case EVENT_TEMP_READY:
    case EVENT_TEMP_INVALID:
        text[0] = '-';
        text[1] = '-';
        if (entry->value >= 1 && entry->value <= 3) {
            memcpy(text, "NTC", 3);
            text[3] = (char)('0' + entry->value);
        }
        break;
    case EVENT_TEMP_INIT_FAILED:
    case EVENT_IO_ERROR:
    case EVENT_DDS_FAILED:
    case EVENT_FAULT_CLEAR_FAILED:
        text[0] = '-';
        text[1] = '-';
        format_number(text, entry->value, 0);
        break;
    default:
        break;
    }
}

static int send_logs(const struct view_snapshot *snapshot, enum dgus_crc crc,
                    view_send_fn send, void *ctx)
{
    uint16_t icons[VIEW_LOG_ROWS];
    for (size_t row = 0; row < VIEW_LOG_ROWS; ++row) {
        icons[row] = (uint16_t)snapshot->logs[row].kind;
    }
    int result = send_words(crc, send, ctx, 0x1010, icons, VIEW_LOG_ROWS);
    for (size_t row = 0; row < VIEW_LOG_ROWS && result == VIEW_OK; ++row) {
        const struct event_entry *entry = &snapshot->logs[row];
        char text[VIEW_TEXT_BYTES] = {0};
        if (entry->kind != EVENT_NONE) {
            text[0] = '-';
            text[1] = '-';
            format_time(text, entry->seconds);
        }
        result = send_text(crc, send, ctx, (uint16_t)(0x1200u + row * 0x20u), text);
        if (result == VIEW_OK) {
            format_log_value(text, entry);
            result = send_text(crc, send, ctx, (uint16_t)(0x1210u + row * 0x20u), text);
        }
    }
    if (result == VIEW_OK) {
        unsigned int first = snapshot->log_count ? snapshot->log_offset + 1u : 0u;
        unsigned int last = snapshot->log_offset + VIEW_LOG_ROWS;
        if (last > snapshot->log_count) {
            last = snapshot->log_count;
        }
        char text[VIEW_TEXT_BYTES] = {
            (char)('0' + first / 10u), (char)('0' + first % 10u), '-',
            (char)('0' + last / 10u), (char)('0' + last % 10u), '/',
            (char)('0' + snapshot->log_count / 10u),
            (char)('0' + snapshot->log_count % 10u), 0};
        result = send_text(crc, send, ctx, 0x1280, text);
    }
    return result;
}

static int send_debug(const struct view_snapshot *s, enum dgus_crc crc, view_send_fn send, void *ctx)
{
    static const char *const relays[] = {"OFF", "K1", "K2 1R", "K3 3R", "K4 10R",
        "K5 30R", "K6 100R", "K7 300R", "K8 1000R"};
    bool dac = s->page == PANEL_PAGE_DAC;
    uint16_t icons[] = {dac ? s->debug.field - 1u : s->debug.field, s->debug.state};
    int rc = send_words(crc, send, ctx, dac ? 0x1030 : 0x1020, icons, 2);
    for (unsigned int i = 0; i < 8 && rc == VIEW_OK; ++i) {
        char text[VIEW_TEXT_BYTES] = {0};
        switch (i) {
        case 0: strcpy(text, relays[s->debug.relay]); break;
        case 1: format_number(text, s->debug.frequency / 1000u, 0); break;
        case 2: format_number(text, s->debug.millivolts_pp, 0); break;
        case 3: strcpy(text, s->debug.state == 2 ? "ON" : s->debug.starting ? "WAIT" : s->debug.pending ? "START?" : "OFF"); break;
        case 4:
            for (unsigned int bit = 0; bit < 8; ++bit) text[bit] = '0' + ((s->debug.coils >> (7u-bit)) & 1u);
            break;
        case 5: format_number(text, s->debug.seconds, 0); break;
        case 6: case 7: {
            enum view_field field = i == 6 ? VIEW_VOLTAGE : VIEW_CURRENT;
            format_value(text, field, s->values[field].value,
                s->fresh && s->state != VIEW_FAULT && s->values[field].valid);
            break;
        }
        }
        rc = send_text(crc, send, ctx, (uint16_t)(0x1300u + i * 0x10u), text);
    }
    return rc;
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
    uint16_t icons[6];
    uint16_t page[2];
    bool online;
    int result;
    if (view == NULL || snapshot == NULL || send == NULL ||
        (unsigned int)snapshot->page > PANEL_PAGE_DAC ||
        (unsigned int)snapshot->state > VIEW_OFFLINE ||
        (unsigned int)snapshot->battery > VIEW_POWER_UNKNOWN ||
        (unsigned int)snapshot->selected > PANEL_OUTPUT ||
        (unsigned int)snapshot->output > VIEW_OUTPUT_CLEARING || snapshot->reason >= VIEW_REASON_COUNT ||
        snapshot->debug.field > 3 || snapshot->debug.relay > 8 || snapshot->debug.state > 5 ||
        (snapshot->page == PANEL_PAGE_DAC && (snapshot->debug.field == 0 || snapshot->debug.relay != 0)) ||
        snapshot->log_count > EVENT_LOG_CAPACITY ||
        snapshot->log_offset > (snapshot->log_count > VIEW_LOG_ROWS ?
                               snapshot->log_count - VIEW_LOG_ROWS : 0u)) {
        return VIEW_ERR_ARG;
    }
    for (size_t row = 0; row < VIEW_LOG_ROWS; ++row) {
        if ((unsigned int)snapshot->logs[row].kind >= EVENT_COUNT) {
            return VIEW_ERR_ARG;
        }
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
    icons[4] = (uint16_t)(online ? snapshot->output : VIEW_OUTPUT_UNAVAILABLE);
    icons[5] = online ? snapshot->reason : OUTPUT_REASON_SCREEN;
    result = send_words(crc, send, ctx, 0x1000, icons, 6);
    if (result == VIEW_OK) {
        result = send_values(snapshot, crc, send, ctx);
    }
    if (result == VIEW_OK && snapshot->page == PANEL_PAGE_LOG) {
        result = send_logs(snapshot, crc, send, ctx);
    }
    if (result == VIEW_OK && snapshot->page >= PANEL_PAGE_DEBUG) {
        result = send_debug(snapshot, crc, send, ctx);
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
