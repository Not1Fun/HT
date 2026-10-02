/* @brief 屏幕与面板联调；可选DAC台架控制，继电器保持全断。 */
#ifndef HT_SCREEN_PANEL_H
#define HT_SCREEN_PANEL_H

/* 数字 GPIO/时钟与继电器全断初始化成功后调用；错误返回给主程序全断锁定。 */
int screen_panel_run(void);

#endif
