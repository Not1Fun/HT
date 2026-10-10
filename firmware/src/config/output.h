/* 功放电压增益与变压器初级额定值，Vpp/RMS换算统一由输出控制器处理。 */
#ifndef HT_OUTPUT_CONFIG_H
#define HT_OUTPUT_CONFIG_H
#define HT_AMP_GAIN_MILLI 15000U
#define HT_PRIMARY_RATED_RMS_MV 14100U
/* 独立工作上限；未确认更高允许值前保持额定值。 */
#define HT_PRIMARY_MAX_RMS_MV 14100U
#endif
