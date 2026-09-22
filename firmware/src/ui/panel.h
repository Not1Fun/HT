/* @brief 已消抖面板事件的设置状态与启停请求模型，不操作硬件。 */
#ifndef PANEL_H
#define PANEL_H

#include <stdbool.h>
#include <stdint.h>

enum panel_result {
    PANEL_OK = 0,
    PANEL_ERR_ARG = -1,
    PANEL_ERR_NOT_READY = -2,
    PANEL_ERR_RANGE = -3
};

enum panel_mode {
    PANEL_CC,
    PANEL_VA
};

enum panel_field {
    PANEL_FREQUENCY,
    PANEL_TARGET,
    PANEL_RANGE
};

enum panel_press {
    PANEL_SHORT_PRESS,
    PANEL_LONG_PRESS
};

enum panel_request {
    PANEL_REQUEST_NONE,
    PANEL_REQUEST_START,
    PANEL_REQUEST_STOP
};

/* 仅为界面请求边界，控制器仍须按实际量程、负载和保护条件二次校验。 */
struct panel_config {
    uint32_t frequency_min_hz;
    uint32_t frequency_max_hz;
    uint32_t frequency_step_hz;
    uint32_t current_max_ma;
    uint32_t current_step_ma;
    uint32_t apparent_max_mva;
    uint32_t apparent_step_mva;
    uint8_t range_count; /* 1..7，range_index 由控制器映射到实际抽头。 */
};

/* 由一个调用者串行维护；外部只读，修改统一经下面的函数。 */
struct panel {
    struct panel_config config;
    enum panel_mode mode;
    enum panel_field field;
    uint32_t frequency_hz;
    uint32_t current_ma;
    uint32_t apparent_mva;
    uint8_t range_index;
    bool auto_range;
    bool enabled;
    bool fault;
    bool armed;
    bool ready;
};

/* 默认 2 kHz、两个目标为 0、自动量程；参数错误使 ready=false。 */
int panel_init(struct panel *panel, const struct panel_config *config,
               enum panel_mode mode, bool enabled, bool fault);
/* detents 是整格计数，正数顺时针；自动量程时旋转量程项不改档。 */
int panel_rotate(struct panel *panel, int32_t detents);
/* 短按依次选择频率/当前模式目标/量程；长按只切自动/手动。 */
int panel_press(struct panel *panel, enum panel_press press);
/* preset_index 0..3 对应 2/5/8/10 kHz，不改变当前选中项。 */
int panel_preset(struct panel *panel, uint8_t preset_index);
/* 输入已消抖，mode 来自物理开关。释放或故障持续返回 STOP。
 * START 仅在有效释放后的合上边沿且当前目标非零时产生一次。
 * 使能合上时换模式（包括同时合上）或目标为零，返回 STOP 并撤销许可。
 * 停止后调大目标、上电已合或故障恢复已合，均须重新释放再合上。
 * 修改目标后应调用本函数检查请求；使能释放时换模式可建立启动许可。
 */
enum panel_request panel_inputs(struct panel *panel, enum panel_mode mode,
                                bool enabled, bool fault);

#endif
