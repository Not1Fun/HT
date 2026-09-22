/* @brief 维护面板参数与释放后启用条件，所有操作只生成软件请求。 */
#include "panel.h"

#include <stddef.h>

static bool valid_mode(enum panel_mode mode)
{
    return mode == PANEL_CC || mode == PANEL_VA;
}

static bool valid_config(const struct panel_config *config)
{
    return config->frequency_min_hz > 0 &&
           config->frequency_min_hz <= 2000 &&
           config->frequency_max_hz >= 2000 &&
           config->frequency_step_hz > 0 &&
           config->frequency_step_hz <= config->frequency_max_hz &&
           config->current_max_ma > 0 && config->current_step_ma > 0 &&
           config->current_step_ma <= config->current_max_ma &&
           config->apparent_max_mva > 0 && config->apparent_step_mva > 0 &&
           config->apparent_step_mva <= config->apparent_max_mva &&
           config->range_count > 0 && config->range_count <= 7;
}

static int check_ready(const struct panel *panel)
{
    if (panel == NULL) {
        return PANEL_ERR_ARG;
    }
    return panel->ready ? PANEL_OK : PANEL_ERR_NOT_READY;
}

static uint32_t adjust(uint32_t value, int32_t detents, uint32_t step,
                       uint32_t minimum, uint32_t maximum)
{
    int64_t next = (int64_t)value + (int64_t)detents * step;

    if (next < minimum) {
        return minimum;
    }
    if (next > maximum) {
        return maximum;
    }
    return (uint32_t)next;
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
    if (!valid_mode(mode) || !valid_config(&copy)) {
        return PANEL_ERR_ARG;
    }

    panel->config = copy;
    panel->mode = mode;
    panel->field = PANEL_FREQUENCY;
    panel->frequency_hz = 2000;
    panel->auto_range = true;
    panel->enabled = enabled;
    panel->fault = fault;
    panel->armed = !enabled && !fault;
    panel->ready = true;
    return PANEL_OK;
}

int panel_rotate(struct panel *panel, int32_t detents)
{
    int result = check_ready(panel);

    if (result != PANEL_OK) {
        return result;
    }
    switch (panel->field) {
    case PANEL_FREQUENCY:
        panel->frequency_hz = adjust(panel->frequency_hz, detents,
                                     panel->config.frequency_step_hz,
                                     panel->config.frequency_min_hz,
                                     panel->config.frequency_max_hz);
        break;
    case PANEL_TARGET:
        if (panel->mode == PANEL_CC) {
            panel->current_ma = adjust(panel->current_ma, detents,
                                       panel->config.current_step_ma, 0,
                                       panel->config.current_max_ma);
        } else {
            panel->apparent_mva = adjust(panel->apparent_mva, detents,
                                         panel->config.apparent_step_mva, 0,
                                         panel->config.apparent_max_mva);
        }
        break;
    case PANEL_RANGE:
        if (!panel->auto_range) {
            panel->range_index = (uint8_t)adjust(panel->range_index, detents,
                                                 1, 0, panel->config.range_count - 1);
        }
        break;
    default:
        return PANEL_ERR_ARG;
    }
    return PANEL_OK;
}

int panel_press(struct panel *panel, enum panel_press press)
{
    int result = check_ready(panel);

    if (result != PANEL_OK) {
        return result;
    }
    if (press == PANEL_SHORT_PRESS) {
        panel->field = panel->field == PANEL_RANGE ? PANEL_FREQUENCY :
                        (enum panel_field)(panel->field + 1);
    } else if (press == PANEL_LONG_PRESS) {
        panel->auto_range = !panel->auto_range;
    } else {
        return PANEL_ERR_ARG;
    }
    return PANEL_OK;
}

int panel_preset(struct panel *panel, uint8_t preset_index)
{
    static const uint32_t frequencies[] = {2000, 5000, 8000, 10000};
    uint32_t frequency;
    int result = check_ready(panel);

    if (result != PANEL_OK) {
        return result;
    }
    if (preset_index >= sizeof(frequencies) / sizeof(frequencies[0])) {
        return PANEL_ERR_ARG;
    }
    frequency = frequencies[preset_index];
    if (frequency < panel->config.frequency_min_hz ||
        frequency > panel->config.frequency_max_hz) {
        return PANEL_ERR_RANGE;
    }
    panel->frequency_hz = frequency;
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
