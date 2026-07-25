#ifndef BUTTON_H
#define BUTTON_H

#include "driver/gpio.h"

/* 按键接线定义：
 * - GPIO0  ：唤醒
 * - GPIO39 ：音量减
 * - GPIO40 ：音量加
 */
#define WAKE_BUTTON_GPIO      GPIO_NUM_0
#define VOL_DOWN_BUTTON_GPIO  GPIO_NUM_39
#define VOL_UP_BUTTON_GPIO    GPIO_NUM_40

/* 系统启动时调用一次，完成按键 GPIO 初始化。 */
void button_init(void);

/* FreeRTOS 按键任务：轮询按键并分发按下事件。 */
void button_task(void *pvParameters);

#endif
