/* 功放电压增益与变压器初级额定值，Vpp/RMS换算统一由输出控制器处理。 */
#ifndef HT_OUTPUT_CONFIG_H
#define HT_OUTPUT_CONFIG_H
#define HT_AMP_GAIN_MILLI 15000U
#define HT_PRIMARY_RATED_RMS_MV 14100U
/* 标称前馈上限；闭环仍按抽头电压、电流和50VA限幅。 */
#define HT_PRIMARY_MAX_RMS_MV 14100U
/* 示波器联调上限，接近DAC量程，实际无削顶摆幅须实测。 */
#define HT_DAC_MAX_MVPP 2800U
#define HT_DAC_TEST_LEVELS {10, 25, 50, 100, 500, 1000, 1500, 2000, 2500, HT_DAC_MAX_MVPP}
#endif
