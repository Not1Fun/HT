/* @brief 两页仪表快照与 DGUS VP 同步，不执行测量或挡位切换。 */
#ifndef VIEW_H
#define VIEW_H
#include "dgus.h"
#include "panel.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIEW_TEXT_BYTES 32u
#define VIEW_TEXT_MAX_CHARS 9u

enum view_result {
    VIEW_OK = 0, VIEW_ERR_ARG = -1, VIEW_ERR_CRC = -2,
    VIEW_ERR_SEND = -3, VIEW_ERR_ENCODING = -4
};
enum view_state {
    VIEW_STANDBY, VIEW_RUNNING, VIEW_SWITCHING, VIEW_FAULT, VIEW_OFFLINE
};
enum view_power { VIEW_POWER_NORMAL, VIEW_POWER_ALARM, VIEW_POWER_UNKNOWN };

/* VP1100起，每槽0x10 word：电流mA、电压mV、阻抗挡Ω、频率挡Hz、
 * 本次运行秒数、候选阻抗Ω、候选频率Hz。频率仅2000/5000/8000/10000。
 */
enum view_field {
    VIEW_CURRENT, VIEW_VOLTAGE, VIEW_RANGE, VIEW_FREQUENCY, VIEW_ELAPSED,
    VIEW_RANGE_CHOICE, VIEW_FREQUENCY_CHOICE, VIEW_FIELD_COUNT
};
struct view_value { int64_t value; bool valid; };

/* 状态页挡位来自控制器已生效状态，不能用面板候选值冒充。
 * elapsed由运行状态拥有者提供，切页不计时、不重置。
 * fresh=false隐藏状态页数据并显示电池未知；故障/切档隐藏电流和电压。
 * 设置页候选值只受各自valid控制，editing表示尚未确认的修改。
 */
struct view_snapshot {
    enum panel_page page;
    enum view_state state;
    enum view_power battery;
    enum panel_field selected;
    bool editing;
    bool fresh;
    struct view_value values[VIEW_FIELD_COUNT];
};
struct view { bool synced; enum panel_page page; };

/* 同步消费一帧，0表示接收成功，不代表屏应答；帧仅在回调中有效。 */
typedef int (*view_send_fn)(void *ctx, const uint8_t *data, size_t length);
/* 每块屏独立上下文；首次、屏复位、重连或CRC配置变化时清同步状态。 */
void view_reset(struct view *view);
/* ui.json v2：4图标+7文本槽，必要时切页。槽完整清尾，同页不重发页命令。
 * 发送失败后下次完整重建。调用者串行调用并处理新鲜度、超时和屏应答。
 */
int view_refresh(struct view *view, const struct view_snapshot *snapshot, enum dgus_crc crc,
                 view_send_fn send, void *ctx);
#endif
