/* @brief 仪表、设置和事件日志快照，不执行测量或挡位切换。 */
#ifndef VIEW_H
#define VIEW_H
#include "dgus.h"
#include "panel.h"
#include "core/event_log.h"
#include "core/output_reason.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIEW_TEXT_BYTES 32u
#define VIEW_TEXT_MAX_CHARS 9u
#define VIEW_LOG_ROWS 4u

enum view_result {
    VIEW_OK = 0, VIEW_ERR_ARG = -1, VIEW_ERR_CRC = -2,
    VIEW_ERR_SEND = -3, VIEW_ERR_ENCODING = -4
};
enum view_state {
    VIEW_STANDBY, VIEW_RUNNING, VIEW_SWITCHING, VIEW_FAULT, VIEW_OFFLINE
};
/* 原因图标末尾对应 POWER_ERROR_IO..MATCH 的7种锁存故障。 */
#define VIEW_REASON_COUNT (OUTPUT_REASON_COUNT + 7u)

enum view_power { VIEW_POWER_NORMAL, VIEW_POWER_ALARM, VIEW_POWER_UNKNOWN };
enum view_output {
    VIEW_OUTPUT_OFF, VIEW_OUTPUT_ARMED, VIEW_OUTPUT_RUNNING,
    VIEW_OUTPUT_UNAVAILABLE, VIEW_OUTPUT_FAULT, VIEW_OUTPUT_DISABLED
};

/* VP1100..1140：电流mA、电压mV、实际阻抗挡Ω、实际频率Hz、本次运行秒数。
 * VP1160：候选频率Hz；VP1170..1190：NTC1..3（0.1°C）；VP11A0：目标mVA。
 * VP1150原手动阻抗候选已停用，其他地址保持不变。
 * 频率仅2000/5000/8000/10000；温度显示范围-20.0..120.0°C。
 */
enum view_field {
    VIEW_CURRENT, VIEW_VOLTAGE, VIEW_RANGE, VIEW_FREQUENCY, VIEW_ELAPSED,
    VIEW_FREQUENCY_CHOICE,
    VIEW_NTC1, VIEW_NTC2, VIEW_NTC3, VIEW_POWER_CHOICE, VIEW_FIELD_COUNT
};
struct view_value { int64_t value; bool valid; };

/* 状态页挡位来自控制器已生效状态，不能用面板候选值冒充。
 * elapsed由运行状态拥有者提供，切页不计时、不重置。
 * fresh=false隐藏状态页数据并显示电池未知；故障隐藏电流和电压。
 * 待机和匹配期间可显示有效测量，不以输出运行状态替代测量有效性。
 * 设置页候选值只受各自valid控制，editing表示尚未确认的修改。
 */
struct view_snapshot {
    enum panel_page page;
    enum view_state state;
    enum view_power battery;
    enum panel_field selected;
    enum view_output output;
    uint8_t reason;
    struct {
        uint8_t field, relay, coils, state;
        uint32_t frequency, seconds;
        uint16_t millivolts_pp;
        bool pending, starting;
    } debug;
    bool editing;
    bool fresh;
    struct view_value values[VIEW_FIELD_COUNT];
    /* 按新到旧填充当前窗口，空行EVENT_NONE；count为全部记录数，offset为窗口起点。 */
    struct event_entry logs[VIEW_LOG_ROWS];
    uint8_t log_count;
    uint8_t log_offset;
};
struct view { bool synced; enum panel_page page; };

/* 同步消费一帧，0表示接收成功，不代表屏应答；帧仅在回调中有效。 */
typedef int (*view_send_fn)(void *ctx, const uint8_t *data, size_t length);
/* 每块屏独立上下文；首次、屏复位、重连或CRC配置变化时清同步状态。 */
void view_reset(struct view *view);
/* ui.json v9：Bench独立DAC页复用VP1300..1370，VP1030/1031为选项和试波状态；VP1005为具体阻止原因。
 * 发送失败后下次完整重建。调用者串行调用并处理新鲜度、超时和屏应答。
 */
int view_refresh(struct view *view, const struct view_snapshot *snapshot, enum dgus_crc crc,
                 view_send_fn send, void *ctx);
#endif
