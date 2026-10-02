/* @brief 验证导航、确认前不生效和独立使能的释放后重启约束。 */
#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static const struct panel_config config = {
    .current_max_ma = 7070,
    .apparent_max_mva = 50000
};

static struct panel initialize(bool enabled, bool fault)
{
    struct panel panel;

    CHECK(panel_init(&panel, &config, PANEL_CC, enabled, fault) == PANEL_OK);
    return panel;
}

static void targets(struct panel *panel)
{
    CHECK(panel_set_target(panel, PANEL_CC, 100) == PANEL_OK);
    CHECK(panel_set_target(panel, PANEL_VA, 1000) == PANEL_OK);
}

static void test_defaults(void)
{
    for (int mode = PANEL_CC; mode <= PANEL_VA; ++mode) {
        struct panel panel;

        CHECK(panel_init(&panel, &config, (enum panel_mode)mode, false, false) == PANEL_OK);
        CHECK(panel.ready && panel.armed && !panel.auto_range);
        CHECK(!panel.enabled && !panel.fault);
        CHECK(!panel.output_running && !panel.output_available);
        CHECK(panel.mode == (enum panel_mode)mode);
        CHECK(panel.page == PANEL_PAGE_STATUS && panel.field == PANEL_RANGE);
        CHECK(panel.range_index == 0 && panel.draft_index == 0 && panel.frequency_hz == 2000);
        CHECK(panel.current_ma == 0 && panel.apparent_mva == 0 && !panel_draft_changed(&panel));
        CHECK(panel_init(&panel, &panel.config, (enum panel_mode)mode, false, false) == PANEL_OK);
        CHECK(panel.config.current_max_ma == 7070 && panel.config.apparent_max_mva == 50000);
    }
}

static void test_tables(void)
{
    static const uint32_t expected_ranges[] = {1, 3, 10, 30, 100, 300, 1000};
    static const uint32_t expected_frequencies[] = {2000, 5000, 8000, 10000};

    CHECK(PANEL_RANGE_COUNT == 7 && PANEL_FREQUENCY_COUNT == 4);
    for (uint8_t i = 0; i < PANEL_RANGE_COUNT; ++i) {
        CHECK(panel_range_ohm(i) == expected_ranges[i]);
    }
    for (uint8_t i = 0; i < PANEL_FREQUENCY_COUNT; ++i) {
        CHECK(panel_frequency_hz(i) == expected_frequencies[i]);
    }
    CHECK(panel_range_ohm(7) == 0 && panel_range_ohm(UINT8_MAX) == 0);
    CHECK(panel_frequency_hz(4) == 0 && panel_frequency_hz(UINT8_MAX) == 0);
}

static void test_key_mapping(void)
{
    static const enum panel_key physical_keys[] = {
        PANEL_KEY_UP, PANEL_KEY_LEFT, PANEL_KEY_OK,
        PANEL_KEY_RIGHT, PANEL_KEY_DOWN, PANEL_KEY_ENCODER
    };

    CHECK(panel_pressed_keys(0xff) == 0);
    CHECK(panel_pressed_keys(0x3f) == 0);
    CHECK(panel_pressed_keys(0x00) == 0x3f);
    for (size_t bit = 0; bit < 6; ++bit) {
        uint8_t mask = (uint8_t)(1u << bit);

        CHECK((size_t)physical_keys[bit] == bit);
        CHECK(panel_pressed_keys((uint8_t)(0xffu ^ mask)) == mask);
    }
    CHECK(panel_pressed_keys(0xee) == 0x11); /* 同时按上/下仍由输入层仲裁。 */
}

static void test_status_navigation(void)
{
    for (int key = PANEL_KEY_UP; key <= PANEL_KEY_ENCODER; ++key) {
        struct panel panel = initialize(false, false);
        bool enters = key == PANEL_KEY_RIGHT || key == PANEL_KEY_OK || key == PANEL_KEY_ENCODER;

        CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK);
        CHECK(panel_key(&panel, (enum panel_key)key) ==
              (key == PANEL_KEY_DOWN ? PANEL_ACTION_OUTPUT_STOP : PANEL_ACTION_NONE));
        CHECK(panel.page == (enters ? PANEL_PAGE_SETTINGS :
                             key == PANEL_KEY_LEFT ? PANEL_PAGE_LOG : PANEL_PAGE_STATUS));
        CHECK(panel.range_index == 0 && panel.frequency_hz == 2000 && panel.draft_index == 0);
        CHECK(panel.mode == PANEL_CC && panel.armed && !panel.enabled);
    }
}

static void test_draft_confirmation(void)
{
    struct panel panel = initialize(false, false);

    CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    CHECK(panel_rotate(&panel, 2) == PANEL_OK);
    CHECK(panel.draft_index == 2 && panel.range_index == 0 && panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_RANGE);
    CHECK(panel.range_index == 2 && !panel.auto_range && !panel_draft_changed(&panel));
    CHECK(panel.page == PANEL_PAGE_SETTINGS && panel.frequency_hz == 2000);
    CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_FREQUENCY && panel.draft_index == 0);
    CHECK(panel_rotate(&panel, 2) == PANEL_OK);
    CHECK(panel.frequency_hz == 2000 && panel.draft_index == 2 && panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_ENCODER) == PANEL_ACTION_FREQUENCY);
    CHECK(panel.frequency_hz == 8000 && panel.range_index == 2 && !panel_draft_changed(&panel));
    CHECK(panel.page == PANEL_PAGE_SETTINGS);
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_FREQUENCY);
    CHECK(!panel.enabled && panel.armed && panel.current_ma == 0 && panel.apparent_mva == 0);
}

static void test_cancel(void)
{
    struct panel panel = initialize(false, false);

    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_NONE);
    CHECK(panel_rotate(&panel, 3) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
    CHECK(panel.page == PANEL_PAGE_STATUS && panel.range_index == 0 && !panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_ENCODER) == PANEL_ACTION_NONE && panel.draft_index == 0);
    CHECK(panel_rotate(&panel, 4) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_ENCODER) == PANEL_ACTION_RANGE);
    CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel_rotate(&panel, 3) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
    CHECK(panel.frequency_hz == 2000 && panel.range_index == 4);
    CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_FREQUENCY && panel.draft_index == 0);
    CHECK(panel_key(&panel, PANEL_KEY_UP) == PANEL_ACTION_NONE && panel.draft_index == 4);
}

static void test_field_selection(void)
{
    struct panel panel = initialize(false, false);

    CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    CHECK(panel_rotate(&panel, 5) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_UP) == PANEL_ACTION_NONE && panel.draft_index == 5);
    CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_FREQUENCY && panel.draft_index == 0 && !panel_draft_changed(&panel));
    CHECK(panel_rotate(&panel, 1) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_POWER && panel.draft_index == 0 && !panel_draft_changed(&panel));
    CHECK(panel_rotate(&panel, 12) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_OUTPUT && panel.draft_index == 0 && !panel_draft_changed(&panel));
    CHECK(panel_rotate(&panel, 1) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_OUTPUT && panel.draft_index == 1);
    CHECK(panel_key(&panel, PANEL_KEY_UP) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_POWER && panel.draft_index == 0 && !panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_UP) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_FREQUENCY && panel.draft_index == 0 && !panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_UP) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_RANGE && panel.draft_index == 0 && !panel_draft_changed(&panel));
    CHECK(panel.range_index == 0 && panel.frequency_hz == 2000 && panel.mode == PANEL_CC);
}

static void select_output(struct panel *panel)
{
    CHECK(panel_key(panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    CHECK(panel_key(panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel_key(panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel_key(panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel->page == PANEL_PAGE_SETTINGS && panel->field == PANEL_OUTPUT);
}

static void test_output_requests(void)
{
    struct panel panel = initialize(false, false);

    select_output(&panel);
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_OUTPUT_STOP);
    CHECK(panel_rotate(&panel, 1) == PANEL_OK && panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_NONE);
    CHECK(!panel.output_running && !panel.output_available);
    CHECK(panel_set_output_state(&panel, false, true) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_ENCODER) == PANEL_ACTION_OUTPUT_START);
    CHECK(!panel.output_running && panel.output_available && panel_draft_changed(&panel));
    CHECK(panel.current_ma == 0 && panel.apparent_mva == 0 && panel.range_index == 0);
    CHECK(!panel.enabled && panel.armed && panel.frequency_hz == 2000);
    CHECK(panel_set_output_state(&panel, true, true) == PANEL_OK);
    CHECK(panel.output_running && panel.draft_index == 1 && !panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_OUTPUT_STOP);
    CHECK(panel.output_running); /* 请求不能提前冒充硬件已停止。 */
    CHECK(panel_set_output_state(&panel, false, true) == PANEL_OK);
    CHECK(panel.draft_index == 0 && !panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_ENCODER) == PANEL_ACTION_OUTPUT_STOP);
}

static void test_output_cancel(void)
{
    struct panel panel = initialize(false, false);

    select_output(&panel);
    CHECK(panel_set_output_state(&panel, false, true) == PANEL_OK);
    CHECK(panel_rotate(&panel, 1) == PANEL_OK);
    CHECK(panel_set_output_state(&panel, false, false) == PANEL_OK);
    CHECK(panel.draft_index == 0 && !panel_draft_changed(&panel));
    CHECK(panel_set_output_state(&panel, false, true) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_OUTPUT_STOP);
    CHECK(panel_rotate(&panel, 1) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
    CHECK(panel.page == PANEL_PAGE_STATUS && panel.draft_index == 0);
    CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_OUTPUT && panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_OUTPUT_STOP);
    CHECK(panel_rotate(&panel, 1) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    CHECK(panel.page == PANEL_PAGE_LOG && panel.draft_index == 0);
    CHECK(panel_key(&panel, PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_OUTPUT_STOP);
    CHECK(panel_rotate(&panel, 1) == PANEL_OK);
    CHECK(panel_set_output_state(&panel, true, true) == PANEL_OK);
    CHECK(panel_set_output_state(&panel, false, true) == PANEL_OK); /* 到期/外部停止。 */
    CHECK(panel.draft_index == 0 && panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_OUTPUT_STOP);
}

static void test_output_stop_priority(void)
{
    for (int available = 0; available <= 1; ++available) {
        for (int draft = 0; draft <= 1; ++draft) {
            for (int encoder = 0; encoder <= 1; ++encoder) {
                struct panel panel = initialize(true, true);

                select_output(&panel);
                CHECK(panel_set_output_state(&panel, true, available != 0) == PANEL_OK);
                CHECK(panel_rotate(&panel, draft ? 0 : -1) == PANEL_OK);
                CHECK(panel_key(&panel, encoder ? PANEL_KEY_ENCODER : PANEL_KEY_OK) ==
                      PANEL_ACTION_OUTPUT_STOP);
                CHECK(panel.output_running && panel.enabled && panel.fault && !panel.armed);
                CHECK(panel_key(&panel, PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
                CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_OUTPUT_STOP);
                CHECK(panel.page == PANEL_PAGE_STATUS && panel.output_running);
            }
        }
    }
}

static void test_output_rotation(void)
{
    struct panel panel = initialize(false, false);

    select_output(&panel);
    CHECK(panel_set_output_state(&panel, false, true) == PANEL_OK);
    CHECK(panel_rotate(&panel, -1) == PANEL_OK && panel.draft_index == 0);
    CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK && panel.draft_index == 0);
    CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK && panel.draft_index == 1);
    CHECK(panel_rotate(&panel, 1) == PANEL_OK && panel.draft_index == 1);
    CHECK(panel_rotate(&panel, 0) == PANEL_OK && panel.draft_index == 1);
    CHECK(!panel.output_running && panel.range_index == 0 && panel.frequency_hz == 2000);
    CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK && panel.draft_index == 0);
    CHECK(panel_key(&panel, PANEL_KEY_UP) == PANEL_ACTION_NONE);
    CHECK(panel_key(&panel, PANEL_KEY_UP) == PANEL_ACTION_NONE);
    CHECK(panel_rotate(&panel, 2) == PANEL_OK);
    CHECK(panel_set_output_state(&panel, true, false) == PANEL_OK);
    CHECK(panel.field == PANEL_FREQUENCY && panel.draft_index == 2);
    CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_OUTPUT && panel.draft_index == 1);
}

static void select_power(struct panel *panel)
{
    CHECK(panel_key(panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    CHECK(panel_key(panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel_key(panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel->field == PANEL_POWER);
}

static void test_power_confirmation(void)
{
    struct panel panel = initialize(false, false);
    select_power(&panel);
    CHECK(panel.draft_index == 0 && panel.apparent_mva == 0);
    CHECK(panel_rotate(&panel, 17) == PANEL_OK);
    CHECK(panel.draft_index == 17 && panel.apparent_mva == 0 && panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_POWER);
    CHECK(panel.apparent_mva == 17000 && !panel_draft_changed(&panel));
    CHECK(panel_rotate(&panel, -2) == PANEL_OK && panel.apparent_mva == 17000);
    CHECK(panel_key(&panel, PANEL_KEY_ENCODER) == PANEL_ACTION_POWER);
    CHECK(panel.apparent_mva == 15000 && !panel_draft_changed(&panel));
    CHECK(panel.current_ma == 0 && panel.range_index == 0 && panel.frequency_hz == 2000);
    CHECK(!panel.output_running && !panel.enabled && panel.page == PANEL_PAGE_SETTINGS);
}

static void test_power_cancel(void)
{
    struct panel panel = initialize(false, false);
    CHECK(panel_set_target(&panel, PANEL_VA, 9000) == PANEL_OK);
    select_power(&panel);
    CHECK(panel.draft_index == 9);
    CHECK(panel_rotate(&panel, 5) == PANEL_OK && panel_draft_changed(&panel));
    CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_OUTPUT && panel.apparent_mva == 9000);
    CHECK(panel_key(&panel, PANEL_KEY_UP) == PANEL_ACTION_NONE);
    CHECK(panel.field == PANEL_POWER && panel.draft_index == 9 && !panel_draft_changed(&panel));
    CHECK(panel_rotate(&panel, 10) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
    CHECK(panel.page == PANEL_PAGE_STATUS && panel.apparent_mva == 9000);
    CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE && panel.draft_index == 9);
    CHECK(panel_rotate(&panel, -9) == PANEL_OK && panel.draft_index == 0);
    CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    CHECK(panel.page == PANEL_PAGE_LOG && panel.apparent_mva == 9000);
    CHECK(panel_key(&panel, PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
    CHECK(panel.draft_index == 9 && !panel_draft_changed(&panel));
}

static void test_power_bounds(void)
{
    const uint32_t limits[] = {50000, 60000, 19999, 999};
    const uint8_t maximum[] = {50, 50, 19, 0};
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        struct panel panel;
        struct panel_config limit = config;
        limit.apparent_max_mva = limits[i];
        CHECK(panel_init(&panel, &limit, PANEL_VA, false, false) == PANEL_OK);
        select_power(&panel);
        CHECK(panel_rotate(&panel, -1) == PANEL_OK && panel.draft_index == 0);
        CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK && panel.draft_index == maximum[i]);
        CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_POWER);
        CHECK(panel.apparent_mva == (uint32_t)maximum[i] * 1000u);
        CHECK(panel_rotate(&panel, 1) == PANEL_OK && panel.draft_index == maximum[i]);
        CHECK(!panel_draft_changed(&panel));
        CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK && panel.draft_index == 0);
        CHECK(panel_key(&panel, PANEL_KEY_ENCODER) == PANEL_ACTION_POWER);
        CHECK(panel.apparent_mva == 0 && !panel_draft_changed(&panel));
        CHECK(!panel.output_running && !panel.enabled);
    }
}

static void test_page_navigation(void)
{
    static const enum panel_page left[] = {PANEL_PAGE_LOG, PANEL_PAGE_STATUS, PANEL_PAGE_SETTINGS};
    static const enum panel_page right[] = {PANEL_PAGE_SETTINGS, PANEL_PAGE_LOG, PANEL_PAGE_STATUS};

    for (int page = PANEL_PAGE_STATUS; page <= PANEL_PAGE_LOG; ++page) {
        for (int direction = 0; direction < 2; ++direction) {
            struct panel panel = initialize(false, false);

            targets(&panel);
            for (int step = 0; step < page; ++step) {
                CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
            }
            CHECK(panel.page == (enum panel_page)page);
            CHECK(panel_rotate(&panel, 3) == PANEL_OK);
            CHECK(panel_key(&panel, direction ? PANEL_KEY_RIGHT : PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
            CHECK(panel.page == (direction ? right[page] : left[page]));
            CHECK(panel.range_index == 0 && panel.frequency_hz == 2000 && panel.draft_index == 0);
            CHECK(!panel_draft_changed(&panel));
            CHECK(panel.current_ma == 100 && panel.apparent_mva == 1000 && panel.armed);
            CHECK(!panel.enabled && !panel.fault && panel.mode == PANEL_CC);
        }
    }
}

static void test_log_controls(void)
{
    struct panel panel = initialize(false, false);
    struct panel original;

    targets(&panel);
    CHECK(panel_key(&panel, PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
    CHECK(panel.page == PANEL_PAGE_LOG);
    original = panel;
    for (int key = PANEL_KEY_UP; key <= PANEL_KEY_ENCODER; ++key) {
        if (key != PANEL_KEY_LEFT && key != PANEL_KEY_RIGHT) {
            CHECK(panel_key(&panel, (enum panel_key)key) == PANEL_ACTION_NONE);
            CHECK(memcmp(&panel, &original, sizeof(panel)) == 0);
        }
    }
    CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK);
    CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK);
    CHECK(memcmp(&panel, &original, sizeof(panel)) == 0);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
    CHECK(panel_inputs(&panel, PANEL_CC, true, true) == PANEL_REQUEST_STOP);
}

static void test_rotation_cycles(void)
{
    struct panel panel = initialize(false, false);

    CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    for (int field = PANEL_RANGE; field <= PANEL_FREQUENCY; ++field) {
        uint8_t count = field == PANEL_RANGE ? 7 : 4;

        for (uint8_t i = 1; i < count; ++i) {
            CHECK(panel_rotate(&panel, 1) == PANEL_OK && panel.draft_index == i);
        }
        CHECK(panel_rotate(&panel, 1) == PANEL_OK && panel.draft_index == 0);
        for (uint8_t i = count; i > 1; --i) {
            CHECK(panel_rotate(&panel, -1) == PANEL_OK && panel.draft_index == i - 1);
        }
        CHECK(panel_rotate(&panel, -1) == PANEL_OK && panel.draft_index == 0);
        CHECK(panel_rotate(&panel, 0) == PANEL_OK && panel.draft_index == 0);
        CHECK(panel_rotate(&panel, 2 * count + 2) == PANEL_OK && panel.draft_index == 2);
        CHECK(panel_rotate(&panel, -2 * count - 2) == PANEL_OK && panel.draft_index == 0);
        CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK);
        CHECK(panel.draft_index == (field == PANEL_RANGE ? 1 : 3));
        CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK);
        CHECK(panel.draft_index == (field == PANEL_RANGE ? 6 : 3));
        CHECK(panel.range_index == 0 && panel.frequency_hz == 2000);
        CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
        CHECK(panel.page == PANEL_PAGE_LOG && panel.draft_index == 0);
        CHECK(panel_key(&panel, PANEL_KEY_LEFT) == PANEL_ACTION_NONE);
        CHECK(panel.page == PANEL_PAGE_SETTINGS && panel.draft_index == 0);
        CHECK(panel_key(&panel, PANEL_KEY_DOWN) == PANEL_ACTION_NONE);
    }
}

static void test_targets(void)
{
    struct panel panel = initialize(false, false);
    struct panel_config large = {UINT32_MAX, UINT32_MAX};

    targets(&panel);
    CHECK(panel_set_target(&panel, PANEL_CC, 7070) == PANEL_OK);
    CHECK(panel_set_target(&panel, PANEL_VA, 50000) == PANEL_OK);
    CHECK(panel_set_target(&panel, PANEL_CC, 7071) == PANEL_ERR_RANGE);
    CHECK(panel_set_target(&panel, PANEL_VA, 50001) == PANEL_ERR_RANGE);
    CHECK(panel_set_target(&panel, PANEL_CC, UINT32_MAX) == PANEL_ERR_RANGE);
    CHECK(panel.current_ma == 7070 && panel.apparent_mva == 50000);
    CHECK(panel.mode == PANEL_CC && panel.page == PANEL_PAGE_STATUS);
    CHECK(panel_init(&panel, &large, PANEL_VA, false, false) == PANEL_OK);
    CHECK(panel_set_target(&panel, PANEL_CC, UINT32_MAX) == PANEL_OK);
    CHECK(panel_set_target(&panel, PANEL_VA, UINT32_MAX) == PANEL_OK);
    CHECK(panel_inputs(&panel, PANEL_VA, true, false) == PANEL_REQUEST_START);
    CHECK(panel_set_target(&panel, PANEL_CC, 0) == PANEL_OK);
    CHECK(panel_inputs(&panel, PANEL_VA, true, false) == PANEL_REQUEST_NONE);
}

static void test_startup_enabled(void)
{
    struct panel panel = initialize(true, false);

    targets(&panel);
    CHECK(!panel.armed);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_key(&panel, PANEL_KEY_RIGHT) == PANEL_ACTION_NONE);
    CHECK(panel_rotate(&panel, 2) == PANEL_OK);
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ACTION_RANGE);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_inputs(&panel, PANEL_CC, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
}

static void test_zero_target(void)
{
    for (int mode = PANEL_CC; mode <= PANEL_VA; ++mode) {
        struct panel panel;

        CHECK(panel_init(&panel, &config, (enum panel_mode)mode, false, false) == PANEL_OK);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_STOP);
        CHECK(!panel.armed);
        targets(&panel);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_NONE);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_START);
        CHECK(panel_set_target(&panel, (enum panel_mode)mode, 0) == PANEL_OK);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_STOP);
        CHECK(!panel.armed);
        targets(&panel);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_NONE);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_START);
    }
}

static void test_fault_rearm(void)
{
    struct panel panel = initialize(false, false);

    targets(&panel);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
    CHECK(panel_inputs(&panel, PANEL_CC, true, true) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_inputs(&panel, PANEL_CC, false, true) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_inputs(&panel, PANEL_CC, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
    panel = initialize(true, true);
    targets(&panel);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_inputs(&panel, PANEL_CC, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
}

static void test_mode_change_stop(void)
{
    for (int mode = PANEL_CC; mode <= PANEL_VA; ++mode) {
        enum panel_mode other = mode == PANEL_CC ? PANEL_VA : PANEL_CC;
        struct panel panel;

        CHECK(panel_init(&panel, &config, (enum panel_mode)mode, false, false) == PANEL_OK);
        targets(&panel);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_START);
        CHECK(panel_inputs(&panel, other, true, false) == PANEL_REQUEST_STOP);
        CHECK(panel.mode == other && !panel.armed);
        CHECK(panel_inputs(&panel, other, true, false) == PANEL_REQUEST_NONE);
        CHECK(panel_inputs(&panel, other, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_STOP);
        CHECK(!panel.armed);
        CHECK(panel_inputs(&panel, other, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel_inputs(&panel, other, true, false) == PANEL_REQUEST_START);
    }
}

static void test_stop_priority(void)
{
    struct panel panel = initialize(false, false);

    targets(&panel);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
    CHECK(panel_inputs(&panel, PANEL_CC, true, true) == PANEL_REQUEST_STOP);
    for (int key = PANEL_KEY_UP; key <= PANEL_KEY_ENCODER; ++key) {
        CHECK(panel_key(&panel, (enum panel_key)key) >= PANEL_ACTION_NONE);
        CHECK(panel_rotate(&panel, 2) == PANEL_OK);
        CHECK(panel_inputs(&panel, PANEL_CC, true, true) == PANEL_REQUEST_STOP);
        CHECK(panel.fault && !panel.armed);
    }
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_inputs(&panel, PANEL_CC, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
}

static void test_invalid_config(void)
{
    struct panel panel = initialize(false, false);

    for (int field = 0; field < 2; ++field) {
        struct panel_config bad = config;

        if (field == 0) bad.current_max_ma = 0;
        else bad.apparent_max_mva = 0;
        CHECK(panel_init(&panel, &bad, PANEL_CC, false, false) == PANEL_ERR_ARG);
        CHECK(!panel.ready && !panel.armed);
        CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_STOP);
    }
}

static void test_invalid_inputs(void)
{
    struct panel panel = initialize(false, false);

    CHECK(panel_init(NULL, &config, PANEL_CC, false, false) == PANEL_ERR_ARG);
    CHECK(panel_key(NULL, PANEL_KEY_OK) == PANEL_ERR_ARG);
    CHECK(panel_rotate(NULL, 1) == PANEL_ERR_ARG);
    CHECK(panel_set_target(NULL, PANEL_CC, 1) == PANEL_ERR_ARG);
    CHECK(panel_set_output_state(NULL, false, false) == PANEL_ERR_ARG);
    CHECK(!panel_draft_changed(NULL));
    CHECK(panel_inputs(NULL, PANEL_CC, true, false) == PANEL_REQUEST_STOP);
    for (int bad = -1; bad <= 99; bad += 100) {
        CHECK(panel_key(&panel, (enum panel_key)bad) == PANEL_ERR_ARG);
        CHECK(panel_set_target(&panel, (enum panel_mode)bad, 1) == PANEL_ERR_ARG);
        CHECK(panel.page == PANEL_PAGE_STATUS && panel.current_ma == 0);
    }
    targets(&panel);
    CHECK(panel_inputs(&panel, (enum panel_mode)99, true, false) == PANEL_REQUEST_STOP);
    CHECK(panel.fault && !panel.armed);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_init(&panel, &config, (enum panel_mode)99, false, false) == PANEL_ERR_ARG);
    CHECK(!panel.ready);
    CHECK(panel_init(&panel, NULL, PANEL_CC, false, false) == PANEL_ERR_ARG);
    CHECK(panel_key(&panel, PANEL_KEY_OK) == PANEL_ERR_NOT_READY);
    CHECK(panel_rotate(&panel, 1) == PANEL_ERR_NOT_READY);
    CHECK(panel_set_target(&panel, PANEL_CC, 1) == PANEL_ERR_NOT_READY);
    CHECK(panel_set_output_state(&panel, true, true) == PANEL_ERR_NOT_READY);
    CHECK(!panel.output_running && !panel.output_available);
    CHECK(!panel_draft_changed(&panel));
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"defaults", test_defaults}, {"tables", test_tables}, {"key_mapping", test_key_mapping},
        {"status_navigation", test_status_navigation}, {"draft_confirmation", test_draft_confirmation},
        {"cancel", test_cancel}, {"field_selection", test_field_selection},
        {"page_navigation", test_page_navigation}, {"log_controls", test_log_controls},
        {"rotation_cycles", test_rotation_cycles}, {"targets", test_targets},
        {"output_requests", test_output_requests}, {"output_cancel", test_output_cancel},
        {"output_stop_priority", test_output_stop_priority}, {"output_rotation", test_output_rotation},
        {"power_confirmation", test_power_confirmation}, {"power_cancel", test_power_cancel},
        {"power_bounds", test_power_bounds},
        {"startup_enabled", test_startup_enabled}, {"zero_target", test_zero_target},
        {"fault_rearm", test_fault_rearm}, {"mode_change_stop", test_mode_change_stop},
        {"stop_priority", test_stop_priority}, {"invalid_config", test_invalid_config},
        {"invalid_inputs", test_invalid_inputs}
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
