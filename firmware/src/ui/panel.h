/* @brief 已消抖面板导航、选档草稿与启停请求模型，不操作硬件。 */
#ifndef PANEL_H
#define PANEL_H

#include <stdbool.h>
#include <stdint.h>

#define PANEL_RANGE_COUNT 7u
#define PANEL_FREQUENCY_COUNT 4u

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

enum panel_page {
    PANEL_PAGE_STATUS = 0,
    PANEL_PAGE_SETTINGS = 1,
    PANEL_PAGE_LOG = 2,
    PANEL_PAGE_DEBUG = 3,
    PANEL_PAGE_DAC = 4
};

enum panel_field {
    PANEL_RANGE = 0,
    PANEL_FREQUENCY = 1,
    PANEL_POWER = 2,
    PANEL_OUTPUT = 3
};

/* 与 TCA9539 Port1 的 bit0..5 对应；bit6/7 为独立使能和门监视。 */
enum panel_key {
    PANEL_KEY_UP,
    PANEL_KEY_LEFT,
    PANEL_KEY_OK,
    PANEL_KEY_RIGHT,
    PANEL_KEY_DOWN,
    PANEL_KEY_ENCODER
};

enum panel_action {
    PANEL_ACTION_NONE = 0,
    PANEL_ACTION_RANGE = 1,
    PANEL_ACTION_FREQUENCY = 2,
    PANEL_ACTION_OUTPUT_START = 3,
    PANEL_ACTION_OUTPUT_STOP = 4,
    PANEL_ACTION_POWER = 5,
    PANEL_ACTION_DEBUG_RELAY, PANEL_ACTION_DEBUG_WAVE, PANEL_ACTION_DEBUG_PARAMS
};

enum panel_request {
    PANEL_REQUEST_NONE,
    PANEL_REQUEST_START,
    PANEL_REQUEST_STOP
};

/* 后台目标边界；控制器仍须按实际量程、负载和保护条件二次校验。 */
struct panel_config {
    uint32_t current_max_ma;
    uint32_t apparent_max_mva;
};

/* 单调用者串行维护；外部只读。阻抗、频率和目标VA是请求，确认后交给输出控制器。 */
struct panel {
    struct panel_config config;
    enum panel_mode mode;
    enum panel_page page;
    enum panel_field field;
    uint8_t draft_index;
    uint8_t range;
    uint32_t frequency_hz;
    uint32_t current_ma;
    uint32_t apparent_mva;
    bool enabled;
    bool fault;
    bool armed;
    bool ready;
    bool output_running;
    bool output_available;
    bool debug_enabled;
    struct { uint8_t field, draft, choice[4]; bool busy, wave; } debug;
};

/* 默认状态页、1 Ω 和 2 kHz 请求、目标为 0；参数错误撤销 ready。 */
int panel_init(struct panel *panel, const struct panel_config *config,
               enum panel_mode mode, bool enabled, bool fault);
/* 每次调用表示一次已消抖按下；返回 panel_action 或负错误。
 * LEFT/RIGHT 按状态、设置、日志循环；Bench增加调试和独立DAC页。
 * 进出调试页发STOP；离开设置丢弃草稿。
 * 状态页 OK/ENCODER 进入设置、DOWN请求停止；设置页UP/DOWN选择阻抗、频率、VA和输出（到头停）。
 * 设置页 OK/ENCODER 提交请求并留页。
 * 日志页其余键不修改模型，由应用层处理浏览。
 */
int panel_key(struct panel *panel, enum panel_key key);
/* 仅设置页有效：阻抗和频率首尾循环；VA与输出预选钳制，不执行硬件操作。 */
int panel_rotate(struct panel *panel, int32_t detents);
bool panel_draft_changed(const struct panel *panel);
/* 同步输出忙碌状态（含匹配/停机）；状态变化或许可撤销取消输出草稿，重获许可不自动开启。 */
int panel_set_output_state(struct panel *panel, bool running, bool available);
/* 表项单位为 ohm/Hz，索引越界返回 0。 */
uint32_t panel_range_ohm(uint8_t index);
uint16_t panel_debug_mvpp(uint8_t index);
uint32_t panel_frequency_hz(uint8_t index);
/* 只做低有效掩码转换，不消抖，不将使能/门监视当作导航键。 */
uint8_t panel_pressed_keys(uint8_t raw_port1);
/* 后台设置指定模式的目标（CC: mA，VA: mVA），不切换模式；超限拒绝。
 * 修改后须立即调用 panel_inputs 检查启停请求。
 */
int panel_set_target(struct panel *panel, enum panel_mode mode, uint32_t value);
/* mode 来自上层配置，不再解释原 P10。释放或故障持续返回 STOP。
 * START 仅在有效释放后的合上边沿且当前目标非零时产生一次。
 * 使能合上时换模式（包括同时合上）或目标为零，STOP 并撤销许可。
 * 停止后调大目标、上电已合、故障恢复已合，均须重新释放再合上。
 */
enum panel_request panel_inputs(struct panel *panel, enum panel_mode mode,
                                bool enabled, bool fault);

#endif
