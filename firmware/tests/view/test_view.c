/* @brief 验证屏幕字段、清尾、动画顺序、故障显示和发送中止行为。 */
#include "view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define DATA_FRAMES (1u + VIEW_FIELD_COUNT)
#define PAGE_FRAME (3u + VIEW_FIELD_COUNT)
#define BASE_FRAMES (4u + VIEW_FIELD_COUNT)
#define ALL_FRAMES (BASE_FRAMES + 1u)

struct capture {
    enum dgus_crc crc;
    size_t count;
    size_t attempts;
    size_t fail_at;
    struct dgus_frame frames[ALL_FRAMES];
};

static int receive(void *ctx, const uint8_t *data, size_t length)
{
    struct capture *capture = ctx;

    if (capture->attempts++ == capture->fail_at) {
        return -1;
    }
    CHECK(capture->count < ALL_FRAMES);
    CHECK(dgus_decode(capture->crc, data, length,
                       &capture->frames[capture->count]) == DGUS_OK);
    CHECK(capture->frames[capture->count].kind == DGUS_WRITE);
    ++capture->count;
    return 0;
}

static struct view_snapshot sample(void)
{
    struct view_snapshot snapshot = {
        .page = VIEW_PAGE_MEASURE,
        .state = VIEW_RUNNING,
        .fault = VIEW_FAULT_UNKNOWN,
        .battery = VIEW_POWER_NORMAL,
        .charger = VIEW_POWER_UNKNOWN,
        .calibration = VIEW_UNCALIBRATED,
        .startup_step = VIEW_STARTUP_WAITING,
        .mode = PANEL_CC,
        .selected = PANEL_TARGET,
        .auto_range = true,
        .fresh = true
    };
    const int64_t values[VIEW_FIELD_COUNT] = {
        12500, 10000, -7500, -369, 12345, 100, 1234, 987,
        2000, 200, 10000, 251, 262, -35, 24000, 2100
    };

    for (size_t field = 0; field < VIEW_FIELD_COUNT; ++field) {
        snapshot.values[field] = (struct view_value){values[field], true};
    }
    return snapshot;
}

static void refresh_existing(struct view *view, struct capture *capture,
                              const struct view_snapshot *snapshot, enum dgus_crc crc)
{
    *capture = (struct capture){.crc = crc, .fail_at = SIZE_MAX};
    CHECK(view_refresh(view, snapshot, crc, receive, capture) == VIEW_OK);
}

static void refresh(struct capture *capture, const struct view_snapshot *snapshot,
                     enum dgus_crc crc)
{
    struct view view = {0};

    refresh_existing(&view, capture, snapshot, crc);
}

static void check_text(const struct capture *capture, enum view_field field,
                        const char *expected)
{
    const struct dgus_frame *frame = NULL;
    unsigned char text[VIEW_TEXT_BYTES];
    size_t length = strlen(expected);

    for (size_t i = 0; i < capture->count; ++i) {
        if (capture->frames[i].vp == 0x1100u + (unsigned int)field * 0x10u) {
            frame = &capture->frames[i];
            break;
        }
    }
    CHECK(frame != NULL);
    CHECK(frame->count == VIEW_TEXT_BYTES / 2);
    for (size_t i = 0; i < VIEW_TEXT_BYTES / 2; ++i) {
        text[2 * i] = (unsigned char)(frame->words[i] >> 8);
        text[2 * i + 1] = (unsigned char)frame->words[i];
    }
    CHECK(length <= VIEW_TEXT_MAX_CHARS);
    CHECK(memcmp(text, expected, length) == 0);
    for (size_t i = length; i < sizeof(text); ++i) {
        CHECK(text[i] == 0);
    }
}

static void test_mapping_sequence(void)
{
    static const uint16_t expected_icons[] = {1, 0, 1, 1, 0, 0, 2, 0, 0};
    struct view_snapshot snapshot = sample();
    struct capture capture;

    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == ALL_FRAMES);
    CHECK(capture.frames[0].vp == 0x1000 && capture.frames[0].count == 1);
    CHECK(capture.frames[0].words[0] == 0);
    CHECK(capture.frames[1].vp == 0x1002 && capture.frames[1].count == 1);
    CHECK(capture.frames[1].words[0] == 0);
    CHECK(capture.frames[2].vp == 0x1004 && capture.frames[2].count == 9);
    CHECK(memcmp(capture.frames[2].words, expected_icons, sizeof(expected_icons)) == 0);
    CHECK(capture.frames[PAGE_FRAME].vp == 0x0084 && capture.frames[PAGE_FRAME].count == 2);
    CHECK(capture.frames[PAGE_FRAME].words[0] == 0x5a01);
    CHECK(capture.frames[PAGE_FRAME].words[1] == VIEW_PAGE_MEASURE);
    CHECK(capture.frames[BASE_FRAMES].vp == 0x1000 && capture.frames[BASE_FRAMES].count == 1);
    CHECK(capture.frames[BASE_FRAMES].words[0] == 1);
    for (size_t i = 0; i < capture.count; ++i) {
        const struct dgus_frame *frame = &capture.frames[i];

        CHECK(!(frame->vp <= 0x1001 && frame->vp + frame->count > 0x1001));
        CHECK(!(frame->vp <= 0x1003 && frame->vp + frame->count > 0x1003));
    }
}

static void test_formatting(void)
{
    static const char *const expected[VIEW_FIELD_COUNT] = {
        "12.500", "10.000", "-7.500", "-36.9", "12.345", "0.100", "1.234", "0.987",
        "2000", "0.200", "10.000", "25.1", "26.2", "-3.5", "24.000", "2100"
    };
    struct view_snapshot snapshot = sample();
    struct capture capture;

    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    for (int field = 0; field < VIEW_FIELD_COUNT; ++field) {
        check_text(&capture, (enum view_field)field, expected[field]);
    }
    snapshot.mode = PANEL_VA;
    snapshot.values[VIEW_R].value = -1;
    snapshot.values[VIEW_X].value = 1;
    snapshot.values[VIEW_PHASE].value = -1;
    snapshot.values[VIEW_TARGET].value = 12345;
    snapshot.values[VIEW_FREQUENCY].value = INT64_C(999999999);
    snapshot.values[VIEW_NTC1].value = 0;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.frames[2].words[1] == PANEL_VA);
    check_text(&capture, VIEW_R, "-0.001");
    check_text(&capture, VIEW_X, "0.001");
    check_text(&capture, VIEW_PHASE, "-0.1");
    check_text(&capture, VIEW_TARGET, "12.345");
    check_text(&capture, VIEW_FREQUENCY, "999999999");
    check_text(&capture, VIEW_NTC1, "0.0");

    snapshot.values[VIEW_FREQUENCY].value = INT64_C(1000000000);
    snapshot.values[VIEW_R].value = -12345678;
    snapshot.values[VIEW_X].value = -1234567;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    check_text(&capture, VIEW_FREQUENCY, "--");
    check_text(&capture, VIEW_R, "--");
    check_text(&capture, VIEW_X, "-1234.567");

    for (size_t field = 0; field < VIEW_FIELD_COUNT; ++field) {
        snapshot.values[field].value = field % 2 == 0 ? INT64_MIN : INT64_MAX;
    }
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    for (int field = 0; field < VIEW_FIELD_COUNT; ++field) {
        check_text(&capture, (enum view_field)field, "--");
    }
}

static void test_field_widths(void)
{
    struct view_snapshot snapshot = sample();
    struct capture capture;

    for (int field = VIEW_NTC1; field <= VIEW_NTC3; ++field) {
        snapshot.values[field].value = 999999;
    }
    snapshot.values[VIEW_BUS].value = 9999999;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    for (int field = VIEW_NTC1; field <= VIEW_NTC3; ++field) {
        check_text(&capture, (enum view_field)field, "99999.9");
    }
    check_text(&capture, VIEW_BUS, "9999.999");

    for (int field = VIEW_NTC1; field <= VIEW_NTC3; ++field) {
        snapshot.values[field].value = 1000000;
    }
    snapshot.values[VIEW_BUS].value = 10000000;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    for (int field = VIEW_NTC1; field <= VIEW_BUS; ++field) {
        check_text(&capture, (enum view_field)field, "--");
    }

    for (int field = VIEW_NTC1; field <= VIEW_NTC3; ++field) {
        snapshot.values[field].value = -99999;
    }
    snapshot.values[VIEW_BUS].value = -999999;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    for (int field = VIEW_NTC1; field <= VIEW_NTC3; ++field) {
        check_text(&capture, (enum view_field)field, "-9999.9");
    }
    check_text(&capture, VIEW_BUS, "-999.999");

    for (int field = VIEW_NTC1; field <= VIEW_NTC3; ++field) {
        snapshot.values[field].value = -100000;
    }
    snapshot.values[VIEW_BUS].value = -1000000;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    for (int field = VIEW_NTC1; field <= VIEW_BUS; ++field) {
        check_text(&capture, (enum view_field)field, "--");
    }
}

static void test_invalid_readings(void)
{
    struct view_snapshot snapshot = sample();
    struct capture capture;

    snapshot.values[VIEW_Z].valid = false;
    snapshot.values[VIEW_TARGET].valid = false;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    check_text(&capture, VIEW_Z, "--");
    check_text(&capture, VIEW_R, "10.000");
    check_text(&capture, VIEW_TARGET, "--");

    for (int condition = 0; condition < 5; ++condition) {
        snapshot = sample();
        if (condition == 0) snapshot.fresh = false;
        if (condition == 1) snapshot.state = VIEW_FAULT;
        if (condition == 2) snapshot.state = VIEW_OFFLINE;
        if (condition == 3) snapshot.page = VIEW_PAGE_FAULT;
        if (condition == 4) snapshot.state = VIEW_SWITCHING;
        refresh(&capture, &snapshot, DGUS_CRC_NONE);
        CHECK(capture.count == BASE_FRAMES);
        for (int field = 0; field < VIEW_FIELD_COUNT; ++field) {
            if (field != VIEW_TARGET && field != VIEW_RANGE && field != VIEW_REQUEST_FREQUENCY) {
                check_text(&capture, (enum view_field)field, "--");
            }
        }
        check_text(&capture, VIEW_TARGET, "0.200");
        check_text(&capture, VIEW_RANGE, "10.000");
        check_text(&capture, VIEW_REQUEST_FREQUENCY, "2100");
    }
}

static void test_states_animations(void)
{
    static const enum view_state expected[3][2][5] = {
        {
            {VIEW_STANDBY, VIEW_STANDBY, VIEW_STANDBY, VIEW_FAULT, VIEW_OFFLINE},
            {VIEW_STANDBY, VIEW_STANDBY, VIEW_STANDBY, VIEW_FAULT, VIEW_OFFLINE}
        },
        {
            {VIEW_OFFLINE, VIEW_OFFLINE, VIEW_OFFLINE, VIEW_FAULT, VIEW_OFFLINE},
            {VIEW_STANDBY, VIEW_RUNNING, VIEW_SWITCHING, VIEW_FAULT, VIEW_OFFLINE}
        },
        {
            {VIEW_FAULT, VIEW_FAULT, VIEW_FAULT, VIEW_FAULT, VIEW_FAULT},
            {VIEW_FAULT, VIEW_FAULT, VIEW_FAULT, VIEW_FAULT, VIEW_FAULT}
        }
    };
    struct capture capture;

    for (int page = VIEW_PAGE_STARTUP; page <= VIEW_PAGE_FAULT; ++page) {
        for (int state = VIEW_STANDBY; state <= VIEW_OFFLINE; ++state) {
            for (int fresh = 0; fresh <= 1; ++fresh) {
                struct view_snapshot snapshot = sample();
                bool running = page == VIEW_PAGE_MEASURE && state == VIEW_RUNNING && fresh != 0;

                snapshot.page = (enum view_page)page;
                snapshot.state = (enum view_state)state;
                snapshot.fresh = fresh != 0;
                snapshot.fault = VIEW_FAULT_TEMPERATURE;
                snapshot.battery = VIEW_POWER_ALARM;
                snapshot.charger = VIEW_POWER_NORMAL;
                snapshot.calibration = VIEW_CALIBRATION_PENDING;
                snapshot.startup_step = VIEW_STARTUP_READY;
                snapshot.auto_range = false;
                snapshot.selected = PANEL_RANGE;
                refresh(&capture, &snapshot, DGUS_CRC_NONE);
                CHECK(capture.count == BASE_FRAMES + (running ? 1u : 0u));
                CHECK(capture.frames[2].words[0] == PANEL_RANGE);
                CHECK(capture.frames[2].words[2] == 0);
                CHECK(capture.frames[2].words[3] == (uint16_t)expected[page][fresh][state]);
                CHECK(snapshot.state == (enum view_state)state);
                CHECK(capture.frames[2].words[4] == VIEW_FAULT_TEMPERATURE);
                CHECK(capture.frames[2].words[5] == VIEW_POWER_ALARM);
                CHECK(capture.frames[2].words[6] == VIEW_POWER_NORMAL);
                CHECK(capture.frames[2].words[7] == VIEW_CALIBRATION_PENDING);
                CHECK(capture.frames[2].words[8] == VIEW_STARTUP_READY);
                CHECK(capture.frames[PAGE_FRAME].words[1] == (uint16_t)page);
                if (running) {
                    CHECK(capture.frames[BASE_FRAMES].vp == 0x1000);
                    CHECK(capture.frames[BASE_FRAMES].words[0] == 1);
                }
            }
        }
    }

    struct view_snapshot snapshot = sample();

    snapshot.page = VIEW_PAGE_STARTUP;
    snapshot.startup_step = VIEW_STARTUP_ERROR;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == BASE_FRAMES);
    CHECK(capture.frames[2].words[3] == VIEW_FAULT);
    CHECK(capture.frames[2].words[8] == VIEW_STARTUP_ERROR);
    check_text(&capture, VIEW_Z, "--");
}

static void test_crc(void)
{
    struct view_snapshot snapshot = sample();
    struct capture plain;
    struct capture crc;

    refresh(&plain, &snapshot, DGUS_CRC_NONE);
    refresh(&crc, &snapshot, DGUS_CRC_MODBUS);
    CHECK(plain.count == crc.count);
    for (size_t i = 0; i < plain.count; ++i) {
        CHECK(plain.frames[i].vp == crc.frames[i].vp);
        CHECK(plain.frames[i].count == crc.frames[i].count);
        CHECK(memcmp(plain.frames[i].words, crc.frames[i].words,
                      plain.frames[i].count * sizeof(uint16_t)) == 0);
    }
}

static void test_send_failure(void)
{
    struct view_snapshot snapshot = sample();

    for (size_t failure = 0; failure < ALL_FRAMES; ++failure) {
        struct view view = {0};
        struct capture capture = {.crc = DGUS_CRC_NONE, .fail_at = failure};

        CHECK(view_refresh(&view, &snapshot, capture.crc, receive, &capture) == VIEW_ERR_SEND);
        CHECK(capture.attempts == failure + 1 && capture.count == failure);
        CHECK(!view.synced);
        refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
        CHECK(capture.count == ALL_FRAMES);
        CHECK(capture.frames[0].vp == 0x1000 && capture.frames[0].words[0] == 0);
        CHECK(capture.frames[1].vp == 0x1002 && capture.frames[1].words[0] == 0);
    }
}

static void test_invalid_args(void)
{
    struct view view = {0};
    struct view_snapshot snapshot = sample();
    struct capture capture = {.crc = DGUS_CRC_NONE, .fail_at = SIZE_MAX};

    view_reset(NULL);
    CHECK(view_refresh(NULL, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
    CHECK(view_refresh(&view, NULL, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
    CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, NULL, &capture) == VIEW_ERR_ARG);
    CHECK(view_refresh(&view, &snapshot, DGUS_CRC_UNCONFIGURED, receive, &capture) == VIEW_ERR_CRC);
    CHECK(view_refresh(&view, &snapshot, (enum dgus_crc)99, receive, &capture) == VIEW_ERR_CRC);
    for (int bad = -1; bad <= 99; bad += 100) {
        snapshot = sample(); snapshot.page = (enum view_page)bad;
        CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
        snapshot = sample(); snapshot.state = (enum view_state)bad;
        CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
        snapshot = sample(); snapshot.fault = (enum view_fault)bad;
        CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
        snapshot = sample(); snapshot.battery = (enum view_power)bad;
        CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
        snapshot = sample(); snapshot.charger = (enum view_power)bad;
        CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
        snapshot = sample(); snapshot.calibration = (enum view_calibration)bad;
        CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
        snapshot = sample(); snapshot.startup_step = (enum view_startup_step)bad;
        CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
        snapshot = sample(); snapshot.mode = (enum panel_mode)bad;
        CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
        snapshot = sample(); snapshot.selected = (enum panel_field)bad;
        CHECK(view_refresh(&view, &snapshot, DGUS_CRC_NONE, receive, &capture) == VIEW_ERR_ARG);
    }
    CHECK(capture.attempts == 0);
}

static void test_continuous_refresh(void)
{
    for (int page = VIEW_PAGE_STARTUP; page <= VIEW_PAGE_FAULT; ++page) {
        struct view view = {0};
        struct view_snapshot snapshot = sample();
        struct capture capture;

        snapshot.page = (enum view_page)page;
        refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
        CHECK(view.synced && view.page == (enum view_page)page);
        for (int repeat = 0; repeat < 5; ++repeat) {
            snapshot.values[VIEW_REQUEST_FREQUENCY].value = 2101 + repeat;
            snapshot.startup_step = VIEW_STARTUP_POWER;
            refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
            CHECK(capture.count == DATA_FRAMES);
            CHECK(capture.frames[0].vp == 0x1004);
            for (size_t i = 0; i < capture.count; ++i) {
                CHECK(capture.frames[i].vp != 0x0084);
                CHECK(capture.frames[i].vp != 0x1000 && capture.frames[i].vp != 0x1002);
            }
        }
    }
}

static void test_transitions(void)
{
    struct view view = {0};
    struct view_snapshot snapshot = sample();
    struct capture capture;

    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    snapshot.state = VIEW_FAULT;
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == DATA_FRAMES + 2);
    CHECK(capture.frames[0].vp == 0x1000 && capture.frames[0].words[0] == 0);
    CHECK(capture.frames[1].vp == 0x1002 && capture.frames[1].words[0] == 0);
    CHECK(capture.frames[2].words[3] == VIEW_FAULT);
    CHECK(!view.run_animation && !view.startup_animation);
    check_text(&capture, VIEW_FREQUENCY, "--");

    snapshot.page = VIEW_PAGE_FAULT;
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == BASE_FRAMES);
    CHECK(capture.frames[PAGE_FRAME].vp == 0x0084);
    CHECK(capture.frames[PAGE_FRAME].words[1] == VIEW_PAGE_FAULT);

    snapshot = sample();
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == ALL_FRAMES && view.run_animation);
    snapshot.fresh = false;
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == DATA_FRAMES + 2);
    CHECK(capture.frames[0].vp == 0x1000 && capture.frames[0].words[0] == 0);
    CHECK(capture.frames[2].words[3] == VIEW_OFFLINE && !view.run_animation);

    snapshot.page = VIEW_PAGE_STARTUP;
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == ALL_FRAMES && view.startup_animation);
    CHECK(capture.frames[BASE_FRAMES].vp == 0x1002);
    snapshot.startup_step = VIEW_STARTUP_ERROR;
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == DATA_FRAMES + 2 && !view.startup_animation);
    CHECK(capture.frames[1].vp == 0x1002 && capture.frames[1].words[0] == 0);
}

static void test_incremental_failure(void)
{
    struct view_snapshot snapshot = sample();

    for (size_t failure = 0; failure < DATA_FRAMES; ++failure) {
        struct view view = {0};
        struct capture capture;

        refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
        capture = (struct capture){.crc = DGUS_CRC_NONE, .fail_at = failure};
        CHECK(view_refresh(&view, &snapshot, capture.crc, receive, &capture) == VIEW_ERR_SEND);
        CHECK(capture.attempts == failure + 1 && !view.synced);
        refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
        CHECK(capture.count == ALL_FRAMES && view.synced);
        CHECK(capture.frames[0].vp == 0x1000 && capture.frames[0].words[0] == 0);
        CHECK(capture.frames[1].vp == 0x1002 && capture.frames[1].words[0] == 0);
        CHECK(capture.frames[PAGE_FRAME].vp == 0x0084);
    }
}

static void test_reset_instances(void)
{
    struct view first = {0};
    struct view second = {0};
    struct view_snapshot snapshot = sample();
    struct capture capture;

    refresh_existing(&first, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == ALL_FRAMES && first.synced && !second.synced);
    refresh_existing(&second, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == ALL_FRAMES && second.synced);
    view_reset(&first);
    CHECK(!first.synced && second.synced);
    refresh_existing(&second, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == DATA_FRAMES);
    refresh_existing(&first, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == ALL_FRAMES);
    CHECK(capture.frames[0].vp == 0x1000 && capture.frames[0].words[0] == 0);
    CHECK(capture.frames[PAGE_FRAME].vp == 0x0084);
}

static void test_request_frequency(void)
{
    struct view_snapshot snapshot = sample();
    struct capture capture;

    snapshot.values[VIEW_FREQUENCY].value = 2101;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    check_text(&capture, VIEW_FREQUENCY, "2101");
    check_text(&capture, VIEW_REQUEST_FREQUENCY, "2100");
    snapshot.fresh = false;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    check_text(&capture, VIEW_FREQUENCY, "--");
    check_text(&capture, VIEW_REQUEST_FREQUENCY, "2100");
    snapshot.values[VIEW_REQUEST_FREQUENCY].valid = false;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    check_text(&capture, VIEW_REQUEST_FREQUENCY, "--");
}

static void test_startup_steps(void)
{
    struct view view = {0};
    struct view_snapshot snapshot = sample();
    struct capture capture;

    snapshot.page = VIEW_PAGE_STARTUP;
    snapshot.fresh = false;
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == ALL_FRAMES && view.startup_animation);
    CHECK(capture.frames[2].words[8] == VIEW_STARTUP_WAITING);
    for (int step = VIEW_STARTUP_POWER; step <= VIEW_STARTUP_RELAY; ++step) {
        snapshot.startup_step = (enum view_startup_step)step;
        refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
        CHECK(capture.count == DATA_FRAMES && view.startup_animation);
        CHECK(capture.frames[0].words[8] == (uint16_t)step);
    }
    snapshot.startup_step = VIEW_STARTUP_READY;
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == DATA_FRAMES + 2 && !view.startup_animation);
    CHECK(capture.frames[1].vp == 0x1002 && capture.frames[1].words[0] == 0);
    CHECK(capture.frames[2].words[8] == VIEW_STARTUP_READY);
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == DATA_FRAMES && !view.startup_animation);

    view_reset(&view);
    snapshot.startup_step = VIEW_STARTUP_ERROR;
    refresh_existing(&view, &capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == BASE_FRAMES && !view.startup_animation);
    CHECK(capture.frames[2].words[3] == VIEW_FAULT);
    CHECK(capture.frames[2].words[8] == VIEW_STARTUP_ERROR);
}

static void test_full_refresh(void)
{
    struct view_snapshot snapshot = sample();
    struct capture capture;

    snapshot.values[VIEW_Z].value = INT64_C(12345678);
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    check_text(&capture, VIEW_Z, "12345.678");
    snapshot.values[VIEW_Z].value = 1;
    snapshot.state = VIEW_STANDBY;
    snapshot.mode = PANEL_VA;
    snapshot.selected = PANEL_FREQUENCY;
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.count == BASE_FRAMES);
    CHECK(capture.frames[2].words[0] == PANEL_FREQUENCY);
    CHECK(capture.frames[2].words[1] == PANEL_VA);
    check_text(&capture, VIEW_Z, "0.001");

    snapshot = (struct view_snapshot){0};
    refresh(&capture, &snapshot, DGUS_CRC_NONE);
    CHECK(capture.frames[2].words[7] == VIEW_UNCALIBRATED);
    CHECK(capture.frames[2].words[8] == VIEW_STARTUP_WAITING);
    check_text(&capture, VIEW_TARGET, "--");
    check_text(&capture, VIEW_RANGE, "--");
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"mapping_sequence", test_mapping_sequence}, {"formatting", test_formatting},
        {"field_widths", test_field_widths},
        {"invalid_readings", test_invalid_readings}, {"states_animations", test_states_animations},
        {"crc", test_crc}, {"send_failure", test_send_failure},
        {"continuous_refresh", test_continuous_refresh}, {"transitions", test_transitions},
        {"incremental_failure", test_incremental_failure}, {"reset_instances", test_reset_instances},
        {"request_frequency", test_request_frequency},
        {"startup_steps", test_startup_steps},
        {"invalid_args", test_invalid_args}, {"full_refresh", test_full_refresh}
    };
    size_t ran = 0;

    CHECK(argc <= 2);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc == 1 || strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            printf("PASS %s\n", cases[i].name);
            ++ran;
        }
    }
    CHECK(ran != 0);
    return EXIT_SUCCESS;
}
