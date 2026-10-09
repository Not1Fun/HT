/* @brief 选档先编辑草稿再提交，导航与输出许可相互独立。 */
#include "panel.h"

#include <stddef.h>

static const uint32_t ranges[PANEL_RANGE_COUNT] = {1, 3, 10, 30, 100, 300, 1000};
static const uint32_t frequencies[PANEL_FREQUENCY_COUNT] = {2000, 5000, 8000, 10000};

uint16_t panel_debug_mvpp(uint8_t index)
{
    static const uint16_t values[] = {10, 25, 50, 100};
    return index < 4 ? values[index] : 0;
}

static uint8_t debug_index(const struct panel *panel)
{
    return panel->debug.field == 3 ? panel->debug.wave : panel->debug.choice[panel->debug.field];
}

static bool valid_mode(enum panel_mode mode)
{
    return mode == PANEL_CC || mode == PANEL_VA;
}

static int check_ready(const struct panel *panel)
{
    if (panel == NULL) {
        return PANEL_ERR_ARG;
    }
    return panel->ready ? PANEL_OK : PANEL_ERR_NOT_READY;
}

static uint8_t selected_index(const struct panel *panel)
{
    if (panel->field == PANEL_OUTPUT) {
        return panel->output_running ? 1u : 0u;
    }
    if (panel->field == PANEL_POWER) return (uint8_t)(panel->apparent_mva / 1000u);
    for (uint8_t index = 0; index < PANEL_FREQUENCY_COUNT; ++index) {
        if (frequencies[index] == panel->frequency_hz) {
            return index;
        }
    }
    return 0;
}

uint32_t panel_range_ohm(uint8_t index)
{
    return index < PANEL_RANGE_COUNT ? ranges[index] : 0;
}

uint32_t panel_frequency_hz(uint8_t index)
{
    return index < PANEL_FREQUENCY_COUNT ? frequencies[index] : 0;
}

uint8_t panel_pressed_keys(uint8_t raw_port1)
{
    return (uint8_t)(~raw_port1 & 0x3fu);
}

int panel_init(struct panel *panel, const struct panel_config *config,
               enum panel_mode mode, bool enabled, bool fault)
{
    struct panel_config copy;

    if (panel == NULL) {
        return PANEL_ERR_ARG;
    }
    if (config == NULL) {
        *panel = (struct panel){0};
        return PANEL_ERR_ARG;
    }
    copy = *config;
    *panel = (struct panel){0};
    if (!valid_mode(mode) || copy.current_max_ma == 0 || copy.apparent_max_mva == 0) {
        return PANEL_ERR_ARG;
    }
    panel->config = copy;
    panel->mode = mode;
    panel->page = PANEL_PAGE_STATUS;
    panel->field = PANEL_FREQUENCY;
    panel->frequency_hz = frequencies[0];
    panel->enabled = enabled;
    panel->fault = fault;
    panel->armed = !enabled && !fault;
    panel->ready = true;
    panel->debug.choice[2] = 1;
    return PANEL_OK;
}

int panel_key(struct panel *panel, enum panel_key key)
{
    int result = check_ready(panel);
    enum panel_field field;

    if (result != PANEL_OK) {
        return result;
    }
    if ((unsigned int)key > PANEL_KEY_ENCODER) {
        return PANEL_ERR_ARG;
    }
    if (key == PANEL_KEY_LEFT || key == PANEL_KEY_RIGHT) {
        enum panel_page before = panel->page;
        enum panel_page last = panel->debug_enabled ? PANEL_PAGE_DAC : PANEL_PAGE_LOG;
        if (key == PANEL_KEY_LEFT) {
            panel->page = panel->page == PANEL_PAGE_STATUS ? last : panel->page - 1;
        } else {
            panel->page = panel->page == last ? PANEL_PAGE_STATUS : panel->page + 1;
        }
        panel->draft_index = selected_index(panel);
        if (before >= PANEL_PAGE_DEBUG || panel->page >= PANEL_PAGE_DEBUG) {
            panel->debug.field = panel->debug.draft = panel->debug.choice[0] = 0;
            if (panel->page == PANEL_PAGE_DAC) {
                panel->debug.field = 1;
                panel->debug.choice[1] = panel->debug.choice[2] = 0;
            }
            return PANEL_ACTION_OUTPUT_STOP;
        }
        return PANEL_ACTION_NONE;
    }
    if (panel->page == PANEL_PAGE_STATUS) {
        if (key == PANEL_KEY_DOWN) {
            return PANEL_ACTION_OUTPUT_STOP;
        }
        if (key == PANEL_KEY_OK || key == PANEL_KEY_ENCODER) {
            panel->draft_index = selected_index(panel);
            panel->page = PANEL_PAGE_SETTINGS;
        }
        return PANEL_ACTION_NONE;
    }
    if (panel->page >= PANEL_PAGE_DEBUG && panel->debug_enabled) {
        if (key == PANEL_KEY_UP || key == PANEL_KEY_DOWN) {
            uint8_t first = panel->page == PANEL_PAGE_DAC ? 1u : 0u;
            if (key == PANEL_KEY_UP && panel->debug.field > first) --panel->debug.field;
            if (key == PANEL_KEY_DOWN && panel->debug.field < 3) ++panel->debug.field;
            panel->debug.draft = debug_index(panel);
        } else if (key == PANEL_KEY_OK || key == PANEL_KEY_ENCODER) {
            unsigned int index = panel->debug.field;
            panel->debug.choice[index] = panel->debug.draft;
            if (index == 0) return panel->debug.draft ? PANEL_ACTION_DEBUG_RELAY : PANEL_ACTION_OUTPUT_STOP;
            if (index == 3) return panel->debug.wave || !panel->debug.draft ?
                PANEL_ACTION_OUTPUT_STOP : PANEL_ACTION_DEBUG_WAVE;
            return PANEL_ACTION_DEBUG_PARAMS;
        }
        return PANEL_ACTION_NONE;
    }
    if (panel->page == PANEL_PAGE_LOG) {
        return PANEL_ACTION_NONE;
    }
    switch (key) {
    case PANEL_KEY_UP:
    case PANEL_KEY_DOWN:
        field = panel->field;
        if (key == PANEL_KEY_UP && field > PANEL_FREQUENCY) {
            --field;
        } else if (key == PANEL_KEY_DOWN && field < PANEL_OUTPUT) {
            ++field;
        }
        if (field != panel->field) {
            panel->field = field;
            panel->draft_index = selected_index(panel);
        }
        break;
    case PANEL_KEY_OK:
    case PANEL_KEY_ENCODER:
        if (panel->field == PANEL_POWER) {
            panel->apparent_mva = (uint32_t)panel->draft_index * 1000u;
            return PANEL_ACTION_POWER;
        }
        if (panel->field == PANEL_OUTPUT) {
            if (panel->output_running || panel->draft_index == 0) {
                return PANEL_ACTION_OUTPUT_STOP;
            }
            return panel->output_available ? PANEL_ACTION_OUTPUT_START : PANEL_ACTION_NONE;
        }
        panel->frequency_hz = frequencies[panel->draft_index];
        return PANEL_ACTION_FREQUENCY;
    default:
        return PANEL_ERR_ARG;
    }
    return PANEL_ACTION_NONE;
}

int panel_rotate(struct panel *panel, int32_t detents)
{
    int result = check_ready(panel);
    int64_t next;
    int64_t count;

    if (result != PANEL_OK) {
        return result;
    }
    if (panel->page >= PANEL_PAGE_DEBUG && panel->debug_enabled) {
        unsigned int field = panel->debug.field;
        count = field == 0 ? 9 : field == 3 ? 2 : 4;
        next = (int64_t)panel->debug.draft + detents;
        if (field == 3) panel->debug.draft = next <= 0 ? 0 : 1;
        else panel->debug.draft = (uint8_t)((next % count + count) % count);
        return PANEL_OK;
    }
    if (panel->page != PANEL_PAGE_SETTINGS) {
        return PANEL_OK;
    }
    if (panel->field == PANEL_OUTPUT) {
        next = (int64_t)panel->draft_index + detents;
        panel->draft_index = next <= 0 ? 0u : 1u;
        return PANEL_OK;
    }
    if (panel->field == PANEL_POWER) {
        next = (int64_t)panel->draft_index + detents;
        uint32_t limit = panel->config.apparent_max_mva / 1000u;
        if (limit > 50) limit = 50;
        panel->draft_index = next < 0 ? 0u : next > limit ? (uint8_t)limit : (uint8_t)next;
        return PANEL_OK;
    }
    count = PANEL_FREQUENCY_COUNT;
    next = ((int64_t)panel->draft_index + detents) % count;
    panel->draft_index = (uint8_t)(next < 0 ? next + count : next);
    return PANEL_OK;
}

bool panel_draft_changed(const struct panel *panel)
{
    return check_ready(panel) == PANEL_OK && panel->page == PANEL_PAGE_SETTINGS &&
           panel->draft_index != selected_index(panel);
}

int panel_set_output_state(struct panel *panel, bool running, bool available)
{
    int rc = check_ready(panel);

    if (rc != PANEL_OK) {
        return rc;
    }
    bool changed = panel->output_running != running;

    panel->output_running = running;
    panel->output_available = available;
    if (panel->field == PANEL_OUTPUT && (changed || !available)) {
        panel->draft_index = running ? 1u : 0u;
    }
    return PANEL_OK;
}

int panel_set_target(struct panel *panel, enum panel_mode mode, uint32_t value)
{
    int result = check_ready(panel);

    if (result != PANEL_OK) {
        return result;
    }
    if (!valid_mode(mode)) {
        return PANEL_ERR_ARG;
    }
    if (value > (mode == PANEL_CC ? panel->config.current_max_ma : panel->config.apparent_max_mva)) {
        return PANEL_ERR_RANGE;
    }
    if (mode == PANEL_CC) {
        panel->current_ma = value;
    } else {
        panel->apparent_mva = value;
    }
    if (mode == panel->mode && value == 0 && panel->enabled) {
        panel->armed = false;
    }
    return PANEL_OK;
}

enum panel_request panel_inputs(struct panel *panel, enum panel_mode mode,
                                bool enabled, bool fault)
{
    bool rising;
    bool mode_changed;
    uint32_t target;

    if (check_ready(panel) != PANEL_OK) {
        return PANEL_REQUEST_STOP;
    }
    rising = enabled && !panel->enabled;
    panel->enabled = enabled;
    if (!valid_mode(mode)) {
        panel->fault = true;
        panel->armed = false;
        return PANEL_REQUEST_STOP;
    }
    mode_changed = mode != panel->mode;
    panel->mode = mode;
    panel->fault = fault;
    if (fault) {
        panel->armed = false;
        return PANEL_REQUEST_STOP;
    }
    if (!enabled) {
        panel->armed = true;
        return PANEL_REQUEST_STOP;
    }
    target = mode == PANEL_CC ? panel->current_ma : panel->apparent_mva;
    if (mode_changed || target == 0) {
        panel->armed = false;
        return PANEL_REQUEST_STOP;
    }
    if (rising && panel->armed) {
        panel->armed = false;
        return PANEL_REQUEST_START;
    }
    return PANEL_REQUEST_NONE;
}
