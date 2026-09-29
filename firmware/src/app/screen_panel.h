/* @brief 输出禁止的屏幕与面板联调服务。 */
#ifndef HT_SCREEN_PANEL_H
#define HT_SCREEN_PANEL_H

/* 数字 GPIO/时钟与继电器全断初始化成功后调用；错误返回给主程序全断锁定。 */
int screen_panel_run(void);

#endif
