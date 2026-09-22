/* @brief 验证面板边界、模式切换和启停请求的释放后重启约束。 */
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
    .frequency_min_hz = 2000,
    .frequency_max_hz = 10000,
    .frequency_step_hz = 100,
    .current_max_ma = 1000,
    .current_step_ma = 10,
    .apparent_max_mva = 50000,
    .apparent_step_mva = 100,
    .range_count = 7
};

static struct panel initialize(bool enabled, bool fault)
{
    struct panel panel;

    CHECK(panel_init(&panel, &config, PANEL_CC, enabled, fault) == PANEL_OK);
    return panel;
}

static void set_target(struct panel *panel)
{
    while (panel->field != PANEL_TARGET) {
        CHECK(panel_press(panel, PANEL_SHORT_PRESS) == PANEL_OK);
    }
    CHECK(panel_rotate(panel, 1) == PANEL_OK);
}

static void test_defaults(void)
{
    for (int mode = PANEL_CC; mode <= PANEL_VA; ++mode) {
        struct panel panel;

        CHECK(panel_init(&panel, &config, (enum panel_mode)mode, false, false) == PANEL_OK);
        CHECK(panel.ready && panel.armed && panel.auto_range);
        CHECK(!panel.enabled && !panel.fault);
        CHECK(panel.mode == (enum panel_mode)mode);
        CHECK(panel.frequency_hz == 2000 && panel.current_ma == 0 && panel.apparent_mva == 0);
        CHECK(panel.field == PANEL_FREQUENCY && panel.range_index == 0);
    }
}

static void test_selection(void)
{
    struct panel panel = initialize(false, false);

    CHECK(panel_press(&panel, PANEL_SHORT_PRESS) == PANEL_OK);
    CHECK(panel.field == PANEL_TARGET);
    CHECK(panel_press(&panel, PANEL_SHORT_PRESS) == PANEL_OK);
    CHECK(panel.field == PANEL_RANGE);
    CHECK(panel_press(&panel, PANEL_SHORT_PRESS) == PANEL_OK);
    CHECK(panel.field == PANEL_FREQUENCY);
    CHECK(panel_press(&panel, PANEL_LONG_PRESS) == PANEL_OK);
    CHECK(!panel.auto_range && panel.field == PANEL_FREQUENCY);
    CHECK(panel.current_ma == 0 && panel.apparent_mva == 0 && !panel.enabled);
}

static void test_rotation(void)
{
    struct panel panel = initialize(false, false);

    CHECK(panel_rotate(&panel, 3) == PANEL_OK);
    CHECK(panel.frequency_hz == 2300);
    CHECK(panel_rotate(&panel, -1) == PANEL_OK);
    CHECK(panel.frequency_hz == 2200);
    CHECK(panel_rotate(&panel, 0) == PANEL_OK);
    CHECK(panel.frequency_hz == 2200);
    CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK);
    CHECK(panel.frequency_hz == 10000);
    CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK);
    CHECK(panel.frequency_hz == 2000);
    CHECK(panel_press(&panel, PANEL_SHORT_PRESS) == PANEL_OK);
    CHECK(panel_rotate(&panel, 5) == PANEL_OK);
    CHECK(panel.current_ma == 50 && panel.apparent_mva == 0);
    CHECK(panel_inputs(&panel, PANEL_VA, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel_rotate(&panel, 3) == PANEL_OK);
    CHECK(panel.apparent_mva == 300 && panel.current_ma == 50);
    CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK);
    CHECK(panel.apparent_mva == 50000);
    CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK);
    CHECK(panel.apparent_mva == 0);
    CHECK(panel_inputs(&panel, PANEL_CC, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel.current_ma == 50);
    CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK);
    CHECK(panel.current_ma == 1000);
    CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK);
    CHECK(panel.current_ma == 0);
}

static void test_presets(void)
{
    static const uint32_t expected[] = {2000, 5000, 8000, 10000};
    struct panel panel = initialize(false, false);
    struct panel_config limited = config;

    set_target(&panel);
    for (uint8_t index = 0; index < 4; ++index) {
        CHECK(panel_preset(&panel, index) == PANEL_OK);
        CHECK(panel.frequency_hz == expected[index] && panel.field == PANEL_TARGET);
    }
    CHECK(panel_preset(&panel, 4) == PANEL_ERR_ARG && panel.frequency_hz == 10000);
    limited.frequency_max_hz = 5000;
    CHECK(panel_init(&panel, &limited, PANEL_CC, false, false) == PANEL_OK);
    CHECK(panel_preset(&panel, 2) == PANEL_ERR_RANGE && panel.frequency_hz == 2000);
}

static void test_range(void)
{
    struct panel panel = initialize(false, false);

    CHECK(panel_press(&panel, PANEL_SHORT_PRESS) == PANEL_OK);
    CHECK(panel_press(&panel, PANEL_SHORT_PRESS) == PANEL_OK);
    CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK && panel.range_index == 0);
    CHECK(panel_press(&panel, PANEL_LONG_PRESS) == PANEL_OK && !panel.auto_range);
    CHECK(panel_rotate(&panel, 2) == PANEL_OK && panel.range_index == 2);
    CHECK(panel_rotate(&panel, -1) == PANEL_OK && panel.range_index == 1);
    CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK && panel.range_index == 6);
    CHECK(panel_press(&panel, PANEL_LONG_PRESS) == PANEL_OK && panel.auto_range);
    CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK && panel.range_index == 6);
    CHECK(panel_press(&panel, PANEL_LONG_PRESS) == PANEL_OK);
    CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK && panel.range_index == 0);
}

static void test_startup_enabled(void)
{
    struct panel panel = initialize(true, false);

    CHECK(!panel.armed);
    set_target(&panel);
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
        set_target(&panel);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_NONE);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel_inputs(&panel, (enum panel_mode)mode, true, false) == PANEL_REQUEST_START);
    }
}

static void test_fault_rearm(void)
{
    struct panel panel = initialize(false, false);

    set_target(&panel);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
    CHECK(panel_inputs(&panel, PANEL_CC, true, true) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_inputs(&panel, PANEL_CC, false, true) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_inputs(&panel, PANEL_CC, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);

    panel = initialize(true, true);
    set_target(&panel);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_inputs(&panel, PANEL_CC, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
}

static void test_mode_change_stop(void)
{
    for (int initial = PANEL_CC; initial <= PANEL_VA; ++initial) {
        enum panel_mode mode = (enum panel_mode)initial;
        enum panel_mode other = mode == PANEL_CC ? PANEL_VA : PANEL_CC;
        struct panel panel;

        CHECK(panel_init(&panel, &config, mode, false, false) == PANEL_OK);
        set_target(&panel);
        CHECK(panel_inputs(&panel, other, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel.armed);
        set_target(&panel);
        CHECK(panel_inputs(&panel, mode, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel.armed);
        CHECK(panel.current_ma > 0 && panel.apparent_mva > 0);
        CHECK(panel_inputs(&panel, mode, true, false) == PANEL_REQUEST_START);
        CHECK(panel_inputs(&panel, other, true, false) == PANEL_REQUEST_STOP);
        CHECK(panel.mode == other && !panel.armed);
        CHECK(panel_inputs(&panel, other, true, false) == PANEL_REQUEST_NONE);
        CHECK(panel_inputs(&panel, other, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel.armed);
        CHECK(panel_inputs(&panel, other, true, false) == PANEL_REQUEST_START);

        CHECK(panel_inputs(&panel, other, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel_inputs(&panel, mode, true, false) == PANEL_REQUEST_STOP);
        CHECK(panel.mode == mode && !panel.armed);
        CHECK(panel_inputs(&panel, mode, true, false) == PANEL_REQUEST_NONE);
        CHECK(panel_inputs(&panel, other, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel.armed);
        CHECK(panel_inputs(&panel, other, true, false) == PANEL_REQUEST_START);
    }
}

static void test_running_zero_stop(void)
{
    for (int initial = PANEL_CC; initial <= PANEL_VA; ++initial) {
        enum panel_mode mode = (enum panel_mode)initial;
        struct panel panel;

        CHECK(panel_init(&panel, &config, mode, false, false) == PANEL_OK);
        set_target(&panel);
        CHECK(panel_inputs(&panel, mode, true, false) == PANEL_REQUEST_START);
        CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK);
        CHECK(panel_inputs(&panel, mode, true, false) == PANEL_REQUEST_STOP);
        CHECK(!panel.armed);
        CHECK(panel_inputs(&panel, mode, true, false) == PANEL_REQUEST_STOP);
        CHECK(panel_rotate(&panel, 1) == PANEL_OK);
        CHECK(panel_inputs(&panel, mode, true, false) == PANEL_REQUEST_NONE);
        CHECK(!panel.armed);
        CHECK(panel_inputs(&panel, mode, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel_inputs(&panel, mode, true, false) == PANEL_REQUEST_START);
    }
}

static void test_stop_priority(void)
{
    struct panel panel = initialize(false, false);

    set_target(&panel);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
    for (int i = 0; i < 3; ++i) {
        CHECK(panel_press(&panel, PANEL_SHORT_PRESS) == PANEL_OK);
        CHECK(panel_press(&panel, PANEL_LONG_PRESS) == PANEL_OK);
        CHECK(panel_rotate(&panel, 2) == PANEL_OK);
        CHECK(panel_preset(&panel, 1) == PANEL_OK);
        CHECK(panel_inputs(&panel, PANEL_VA, false, false) == PANEL_REQUEST_STOP);
        CHECK(panel_inputs(&panel, PANEL_CC, true, true) == PANEL_REQUEST_STOP);
        CHECK(panel.fault && !panel.armed);
    }
}

static void expect_invalid(struct panel_config bad)
{
    struct panel panel = initialize(false, false);

    set_target(&panel);
    CHECK(panel_init(&panel, &bad, PANEL_CC, false, false) == PANEL_ERR_ARG);
    CHECK(!panel.ready && !panel.armed && panel.current_ma == 0);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_STOP);
    CHECK(panel_rotate(&panel, 1) == PANEL_ERR_NOT_READY);
}

static void test_invalid_config(void)
{
    struct panel_config bad = config;

    bad.frequency_min_hz = 0; expect_invalid(bad); bad = config;
    bad.frequency_min_hz = 2001; expect_invalid(bad); bad = config;
    bad.frequency_max_hz = 1999; expect_invalid(bad); bad = config;
    bad.frequency_step_hz = 0; expect_invalid(bad); bad = config;
    bad.frequency_step_hz = 10001; expect_invalid(bad); bad = config;
    bad.current_max_ma = 0; expect_invalid(bad); bad = config;
    bad.current_step_ma = 0; expect_invalid(bad); bad = config;
    bad.current_step_ma = 1001; expect_invalid(bad); bad = config;
    bad.apparent_max_mva = 0; expect_invalid(bad); bad = config;
    bad.apparent_step_mva = 0; expect_invalid(bad); bad = config;
    bad.apparent_step_mva = 50001; expect_invalid(bad); bad = config;
    bad.range_count = 0; expect_invalid(bad); bad = config;
    bad.range_count = 8; expect_invalid(bad);
}

static void test_invalid_inputs(void)
{
    struct panel panel = initialize(false, false);

    CHECK(panel_init(NULL, &config, PANEL_CC, false, false) == PANEL_ERR_ARG);
    CHECK(panel_rotate(NULL, 1) == PANEL_ERR_ARG);
    CHECK(panel_press(NULL, PANEL_SHORT_PRESS) == PANEL_ERR_ARG);
    CHECK(panel_preset(NULL, 0) == PANEL_ERR_ARG);
    CHECK(panel_inputs(NULL, PANEL_CC, true, false) == PANEL_REQUEST_STOP);
    CHECK(panel_init(&panel, NULL, PANEL_CC, false, false) == PANEL_ERR_ARG && !panel.ready);
    CHECK(panel_press(&panel, PANEL_SHORT_PRESS) == PANEL_ERR_NOT_READY);
    CHECK(panel_preset(&panel, 0) == PANEL_ERR_NOT_READY);
    CHECK(panel_init(&panel, &config, (enum panel_mode)99, false, false) == PANEL_ERR_ARG);
    CHECK(!panel.ready);
    panel = initialize(false, false);
    set_target(&panel);
    CHECK(panel_inputs(&panel, (enum panel_mode)99, true, false) == PANEL_REQUEST_STOP);
    CHECK(panel.fault && !panel.armed);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_NONE);
    CHECK(panel_inputs(&panel, PANEL_CC, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel_inputs(&panel, PANEL_CC, true, false) == PANEL_REQUEST_START);
    CHECK(panel_press(&panel, (enum panel_press)99) == PANEL_ERR_ARG);
    CHECK(panel.field == PANEL_TARGET && panel.auto_range);
}

static void test_extremes(void)
{
    struct panel_config large = config;
    struct panel panel;

    large.frequency_max_hz = UINT32_MAX;
    large.frequency_step_hz = UINT32_MAX;
    large.current_max_ma = UINT32_MAX;
    large.current_step_ma = UINT32_MAX;
    large.apparent_max_mva = UINT32_MAX;
    large.apparent_step_mva = UINT32_MAX;
    CHECK(panel_init(&panel, &large, PANEL_CC, false, false) == PANEL_OK);
    for (int field = 0; field < 2; ++field) {
        CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK);
        CHECK((field == 0 ? panel.frequency_hz : panel.current_ma) == UINT32_MAX);
        CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK);
        CHECK((field == 0 ? panel.frequency_hz : panel.current_ma) == UINT32_MAX);
        CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK);
        CHECK((field == 0 ? panel.frequency_hz : panel.current_ma) == (field == 0 ? 2000U : 0U));
        if (field == 0) {
            CHECK(panel_press(&panel, PANEL_SHORT_PRESS) == PANEL_OK);
        }
    }
    CHECK(panel_inputs(&panel, PANEL_VA, false, false) == PANEL_REQUEST_STOP);
    CHECK(panel_rotate(&panel, INT32_MAX) == PANEL_OK && panel.apparent_mva == UINT32_MAX);
    CHECK(panel_rotate(&panel, INT32_MIN) == PANEL_OK && panel.apparent_mva == 0);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"defaults", test_defaults}, {"selection", test_selection},
        {"rotation", test_rotation}, {"presets", test_presets},
        {"range", test_range}, {"startup_enabled", test_startup_enabled},
        {"zero_target", test_zero_target}, {"fault_rearm", test_fault_rearm},
        {"mode_change_stop", test_mode_change_stop}, {"running_zero_stop", test_running_zero_stop},
        {"stop_priority", test_stop_priority}, {"invalid_config", test_invalid_config},
        {"invalid_inputs", test_invalid_inputs}, {"extremes", test_extremes}
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
