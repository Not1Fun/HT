/* @brief 可配置整格转换数的 A/B 正交解码，只生成旋转格数。 */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdbool.h>
#include <stdint.h>

enum encoder_result {
    ENCODER_OK = 0,
    ENCODER_ERR_ARG = -1,
    ENCODER_ERR_NOT_READY = -2
};

struct encoder_config {
    uint8_t transitions_per_detent; /* 仅 2 或 4，须对应实物定位与脉冲关系。 */
    bool reversed;
};

/* 单调用者串行维护，外部只读；不依赖 GPIO、定时器、OS 或动态内存。 */
struct encoder {
    struct encoder_config config;
    uint8_t previous;
    int8_t partial;
    bool synced;
    bool ready;
};

/* ab=(A<<1)|B，取当前稳定定位处的电平；初始化不产生事件。
 * 默认 00→01→11→10→00 为正方向；是否为右旋必须实测后选择 reversed。
 */
int encoder_init(struct encoder *encoder, const struct encoder_config *config, uint8_t ab);

/* 每次输入稳定的两位电平，detents 返回 -1/0/+1；可写输出在错误时置 0。
 * 未完成整格的回摆互相抵消；跨两位跳变清除部分累计并以新电平同步。
 * ab>3 使同步失效，下个合法样本仅重建基线。调用者须避免遗漏转换。
 * 这是状态序列解码，不是按时间消抖；完整合法周期无法区分真实转动与噪声。
 * ENTER 由面板适配层消抖后交给 panel_key(ENCODER)，本模块不处理按下。
 */
int encoder_update(struct encoder *encoder, uint8_t ab, int8_t *detents);

#endif
