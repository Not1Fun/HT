/** @brief 采样链标称参数，单位写入名称；实机换算仍须标定。 */
#ifndef HT_ANALOG_H
#define HT_ANALOG_H

/* 2026-09-22 用户确认的装配值。 */
#define HT_SHUNT_UOHM       20000U
#define HT_DIV_LOW_OHM      806U
#define HT_NTC_R25_OHM      10000U

/* 沿用电路设计，不能替代实物核验。 */
#define HT_DIV_HIGH_OHM     (7U * 51000U)
#define HT_NTC_BETA_K       3950U
#define HT_NTC_PULLUP_OHM   22000U
#define HT_VREF_MV          2900U

#endif
