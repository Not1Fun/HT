/* @brief 同步定点界面快照与 DGUS 页/动画状态，不连接串口或执行控制。 */
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
    VIEW_OK = 0,
    VIEW_ERR_ARG = -1,
    VIEW_ERR_CRC = -2,
    VIEW_ERR_SEND = -3,
    VIEW_ERR_ENCODING = -4
};

enum view_page {
    VIEW_PAGE_STARTUP = 0,
    VIEW_PAGE_MEASURE = 1,
    VIEW_PAGE_FAULT = 2
};

enum view_state {
    VIEW_STANDBY = 0,
    VIEW_RUNNING = 1,
    VIEW_SWITCHING = 2,
    VIEW_FAULT = 3,
    VIEW_OFFLINE = 4
};

enum view_fault {
    VIEW_FAULT_UNKNOWN = 0,
    VIEW_FAULT_CURRENT = 1,
    VIEW_FAULT_VOLTAGE = 2,
    VIEW_FAULT_TEMPERATURE = 3,
    VIEW_FAULT_SENSOR = 4,
    VIEW_FAULT_POWER = 5,
    VIEW_FAULT_COMMUNICATION = 6
};

enum view_power {
    VIEW_POWER_NORMAL = 0,
    VIEW_POWER_ALARM = 1,
    VIEW_POWER_UNKNOWN = 2
};

enum view_calibration {
    VIEW_UNCALIBRATED = 0,
    VIEW_CALIBRATED = 1,
    VIEW_CALIBRATION_PENDING = 2
};

/* 仅显示调用者提供的启动步骤，不执行或按时间推断自检通过。 */
enum view_startup_step {
    VIEW_STARTUP_WAITING = 0,
    VIEW_STARTUP_POWER = 1,
    VIEW_STARTUP_PROTECTION = 2,
    VIEW_STARTUP_RELAY = 3,
    VIEW_STARTUP_READY = 4,
    VIEW_STARTUP_ERROR = 5
};

/* 按以下顺序从 VP1100 起，每项占 0x10 word；单位由页面背景显示。
 * Z/R/X/range 为毫欧，phase 为 0.1 度，V/bus 为毫伏，I 为毫安，
 * VA/W 为毫单位，frequency/request_frequency 为实际/请求 Hz，target 为
 * CC 毫安或 VA 毫VA，NTC 为 0.1 摄氏度。三位小数，phase/NTC 一位，频率整数。
 */
enum view_field {
    VIEW_Z,
    VIEW_R,
    VIEW_X,
    VIEW_PHASE,
    VIEW_VOLTAGE,
    VIEW_CURRENT,
    VIEW_APPARENT,
    VIEW_ACTIVE,
    VIEW_FREQUENCY,
    VIEW_TARGET,
    VIEW_RANGE,
    VIEW_NTC1,
    VIEW_NTC2,
    VIEW_NTC3,
    VIEW_BUS,
    VIEW_REQUEST_FREQUENCY,
    VIEW_FIELD_COUNT
};

struct view_value {
    int64_t value;
    bool valid;
};

/* 状态、有效性与新鲜度由调用者提供，本模块不判断输出是否安全。
 * fresh=false、故障页、故障/离线/切档状态使实测项显示 --；target/range/
 * request_frequency 是设置项，仅受各自 valid 影响。有符号值保留负号。
 * 展示状态优先显示故障页/故障/启动异常，其次离线或测量数据陈旧；
 * 其余启动页只显示待机，避免矛盾快照显示运行。不会修改传入快照。
 * charger 表示充电器正常/报警/未知，正常不代表正在充电。
 */
struct view_snapshot {
    enum view_page page;
    enum view_state state;
    enum view_fault fault;
    enum view_power battery;
    enum view_power charger;
    enum view_calibration calibration;
    enum view_startup_step startup_step;
    enum panel_mode mode;
    enum panel_field selected;
    bool auto_range;
    bool fresh;
    struct view_value values[VIEW_FIELD_COUNT];
};

/* 每块屏独立维护；首次置零或调用 view_reset，外部不得修改同步缓存。 */
struct view {
    bool synced;
    enum view_page page;
    enum view_state state;
    bool run_animation;
    bool startup_animation;
};

/* 屏复位、断线/重连或 CRC 配置变化时调用；只清缓存，不发送数据。 */
void view_reset(struct view *view);

/* 同步消费完整帧，返回 0 表示接收成功，非 0 中止本次刷新；不代表屏幕应答。
 * data 仅在回调期间有效。调用者串行发送，期间不得修改 view/snapshot。
 */
typedef int (*view_send_fn)(void *ctx, const uint8_t *data, size_t length);

/* 显式选择与屏一致的 CRC；首次/失同步时停止动画、全量字段、切页、启动。
 * 后续同页同状态只更新字段；页/展示状态改变先停动画，仅页改变时切页。
 * 启动 READY/ERROR 停止启动动画；WAITING 显示等待步骤，不自动推断通过。
 * 动画仅写 VP1000/1002 的单字，跳过屏保留的 VP1001/1003。
 * 故障或离线不启动运行动画；发送失败立即中止并失同步，下次完整重建。
 * 原生动画断线后不会自行停止，不能当作主控存活或安全状态证明。
 * ASCII 每槽完整写 32 字节；NTC 最多 7 字符、bus 最多 8，其余 9，余部为 0。
 */
int view_refresh(struct view *view, const struct view_snapshot *snapshot, enum dgus_crc crc,
                 view_send_fn send, void *ctx);

#endif
