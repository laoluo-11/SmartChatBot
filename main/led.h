#ifndef LED_H
#define LED_H

#define LED_GPIO        48
#define LED_ACTIVE_HIGH  1  //板载 RGB 灯,低电平点亮
void led_task(void *pvParameters);

#endif
