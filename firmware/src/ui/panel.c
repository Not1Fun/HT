/* @brief 选档先编辑草稿再提交，导航与输出许可相互独立。 */
#include "panel.h"

#include <stddef.h>

static const uint32_t ranges[PANEL_RANGE_COUNT] = {1, 3, 10, 30, 100, 300, 1000};
static const uint32_t frequencies[PANEL_FREQUENCY_COUNT] = {2000, 5000, 8000, 10000};

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
    if (panel->field == PANEL_RANGE) {
        return panel->range_index;
    }
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
    panel->field = PANEL_RANGE;
    panel->frequency_hz = frequencies[0];
    panel->enabled = enabled;
    panel->fault = fault;
    panel->armed = !enabled && !fault;
    panel->ready = true;
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
    if (panel->page == PANEL_PAGE_STATUS) {
        if (key == PANEL_KEY_RIGHT || key == PANEL_KEY_OK || key == PANEL_KEY_ENCODER) {
            panel->draft_index = selected_index(panel);
            panel->page = PANEL_PAGE_SETTINGS;
        }
        return PANEL_ACTION_NONE;
    }
    switch (key) {
    case PANEL_KEY_LEFT:
        panel->draft_index = selected_index(panel);
        panel->page = PANEL_PAGE_STATUS;
        break;
    case PANEL_KEY_UP:
    case PANEL_KEY_DOWN:
        field = key == PANEL_KEY_UP ? PANEL_RANGE : PANEL_FREQUENCY;
        if (field != panel->field) {
            panel->field = field;
            panel->draft_index = selected_index(panel);
        }
        break;
    case PANEL_KEY_OK:
    case PANEL_KEY_ENCODER:
        if (panel->field == PANEL_RANGE) {
            panel->range_index = panel->draft_index;
            panel->auto_range = false;
            return PANEL_ACTION_RANGE;
        }
        panel->frequency_hz = frequencies[panel->draft_index];
        return PANEL_ACTION_FREQUENCY;
    case PANEL_KEY_RIGHT:
        break;
    default:
        return PANEL_ERR_ARG;
    }
    return PANEL_ACTION_NONE;
}

int panel_rotate(struct panel *panel, int32_t detents)
{
    int result = check_ready(panel);
    int64_t next;
    uint8_t maximum;

    if (result != PANEL_OK) {
        return result;
    }
    if (panel->page == PANEL_PAGE_STATUS) {
        return PANEL_OK;
    }
    maximum = panel->field == PANEL_RANGE ? PANEL_RANGE_COUNT - 1 : PANEL_FREQUENCY_COUNT - 1;
    next = (int64_t)panel->draft_index + detents;
    panel->draft_index = next < 0 ? 0 : next > maximum ? maximum : (uint8_t)next;
    return PANEL_OK;
}

bool panel_draft_changed(const struct panel *panel)
{
    return check_ready(panel) == PANEL_OK && panel->page == PANEL_PAGE_SETTINGS &&
           (panel->draft_index != selected_index(panel) ||
            (panel->field == PANEL_RANGE && panel->auto_range));
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
