#include "led.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "led";

void led_task(void *pvParameters)
{
    (void)pvParameters; // 本任务不接收外部参数，消除"未使用参数"的编译警告
    gpio_reset_pin(LED_GPIO); // 把该脚复位成默认状态（清掉上电残留配置）
    gpio_set_direction(LED_GPIO,GPIO_MODE_OUTPUT);// 设置成"输出模式"——我们要用它去控制 LED 亮灭

    while(1)
    {
        gpio_set_level(LED_GPIO, LED_ACTIVE_HIGH ? 0 : 1);  // 先点亮（电平，因为 LED_ACTIVE_LOW=0）
        vTaskDelay(pdMS_TO_TICKS(500));                      // 延时 500 毫秒（任务延时，不会卡住整个系统）
        //gpio_set_level(LED_GPIO, LED_ACTIVE_HIGH ? 1 : 0);  // 再熄灭
        //vTaskDelay(pdMS_TO_TICKS(500));                      // 再等 500 毫秒 → 合计 1 秒闪一次
    }


}