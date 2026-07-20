/* =========================================================================
 * button.c —— 三个按键的检测（L5 改造：不同于单键版）
 * -------------------------------------------------------------------------
 * 思路（标准 FreeRTOS 做法）：
 *   按键按下 -> GPIO 下降沿 -> 进中断(ISR) -> 只往队列丢一个"哪个脚被按了"
 *   -> button_task 从队列取出 -> 软件消抖 -> 把 gpio 映射成动作 -> 调用回调
 *
 * 三个按键共用同一套 ISR/队列/任务，区别只在"哪个 gpio 触发"。所以 ISR 用
 * 参数 arg 把 gpio 号带进来，入队时把 gpio 号一起丢进队列；button_task 取出
 * gpio 后，先消抖，再查表把它翻译成 button_action_t，交给上层。
 *
 * 为什么不在中断里直接处理？
 *   中断服务程序(ISR)要求"快进快出"——不能做耗时操作、不能调 printf、
 *   也不能调会阻塞的函数。所以中断里只负责发个信号到队列，
 *   真正耗时的活（消抖、操作 OLED/LED/喇叭）交给普通的 button_task 去做。
 * ========================================================================= */

#include "button.h"
#include "driver/gpio.h"        // gpio_config / 中断相关 API
#include "esp_attr.h"           // IRAM_ATTR：把中断函数放进 RAM，运行更稳
#include "freertos/queue.h"     // 队列：在中断和任务之间传递"按下"事件
#include "esp_log.h"
#include <stdint.h>             // intptr_t：把 gpio 号安全地塞进 void* 参数

static const char *TAG = "button";

/* 中断和任务之间的"邮箱"：队列里放一个 int（被按下的 gpio 号） */
static QueueHandle_t s_btn_queue = NULL;

/* 按键按下时要执行的回调（由 main.c 注册，里面按动作分派） */
static void (*s_btn_cb)(button_action_t action) = NULL;

/* gpio 号 -> 动作 的对照表（下标就是 button_action_t 的值）。
 * 这样 button_task 只需"按 gpio 找它在表里第几个"，就得到了动作。 */
static const int s_btn_gpio[BTN_ACTION_COUNT] = {
    [BTN_WAKE]      = BTN_WAKE_GPIO,
    [BTN_VOL_DOWN]  = BTN_VOL_DOWN_GPIO,
    [BTN_VOL_UP]    = BTN_VOL_UP_GPIO,
};

/* 中断服务程序（ISR）：尽量短，只往队列丢"被按下的 gpio 号" */
static void IRAM_ATTR button_isr(void *arg)
{
    int gpio = (int)(intptr_t)arg;          // 注册 ISR 时把 gpio 号当参数传进来了
    BaseType_t higher_woken = pdFALSE;
    /* 从 ISR 里发队列：必须用 FromISR 版本；higher_woken 标记是否有更高优先级任务被唤醒 */
    xQueueSendFromISR(s_btn_queue, &gpio, &higher_woken);
    if (higher_woken == pdTRUE) {
        portYIELD_FROM_ISR();   // 如果有，立刻切到那个任务（不等当前 ISR 结束）
    }
}

void button_init(void)
{
    /* 把三个 gpio 拼成一个位掩码（一次配置三个脚） */
    uint64_t mask = 0;
    for (int i = 0; i < BTN_ACTION_COUNT; i++) {
        mask |= (1ULL << s_btn_gpio[i]);
    }

    /* 配置：输入 + 内部上拉 + 下降沿中断（按下=高->低） */
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     // 内部上拉：没按时读到高电平
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,      // 下降沿：高(1)->低(0) 的瞬间触发
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* 建队列：深度 4，元素大小 sizeof(int) —— 存"哪个脚被按了" */
    s_btn_queue = xQueueCreate(4, sizeof(int));

    /* 安装 GPIO 中断服务，并给每个按键单独挂 ISR（arg 传各自的 gpio 号） */
    gpio_install_isr_service(0);
    for (int i = 0; i < BTN_ACTION_COUNT; i++) {
        gpio_isr_handler_add(s_btn_gpio[i], button_isr, (void *)(intptr_t)s_btn_gpio[i]);
    }

    ESP_LOGI(TAG, "按键已就绪 (唤醒=GPIO%d, 音量-=GPIO%d, 音量+=GPIO%d, 下降沿触发)",
             BTN_WAKE_GPIO, BTN_VOL_DOWN_GPIO, BTN_VOL_UP_GPIO);
}

void button_register_callback(void (*cb)(button_action_t action))
{
    s_btn_cb = cb;
}

/* 把 gpio 号翻译成动作；找不到（理论不会发生）就当唤醒处理 */
static button_action_t gpio_to_action(int gpio)
{
    for (int i = 0; i < BTN_ACTION_COUNT; i++) {
        if (s_btn_gpio[i] == gpio) {
            return (button_action_t)i;
        }
    }
    return BTN_WAKE;
}

void button_task(void *pvParameters)
{
    (void)pvParameters;   // 本任务不接收外部参数
    int gpio;
    while (1) {
        /* 一直等"按下"事件（没有就睡，不占 CPU） */
        if (xQueueReceive(s_btn_queue, &gpio, portMAX_DELAY)) {
            /* 软件消抖：等 50ms 再看脚电平，还是低(0)才认定真按下。
             * 机械按键按下瞬间弹簧会弹几下（几毫秒内高高低低），不消抖会一次按当成好几次。 */
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(gpio) == 0) {
                button_action_t act = gpio_to_action(gpio);
                ESP_LOGI(TAG, "按键 GPIO%d -> 动作 %d", gpio, act);
                if (s_btn_cb != NULL) {
                    s_btn_cb(act);   // 在任务上下文调用，可以安全操作 OLED/LED/喇叭
                }
            }
        }
    }
}
