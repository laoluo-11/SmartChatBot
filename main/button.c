#include "button.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_out.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 当前按键按“低电平按下、内部上拉”接法处理。 */
#define BUTTON_ACTIVE_LEVEL    0
#define BUTTON_POLL_MS         20
#define BUTTON_DEBOUNCE_COUNT  3
#define BUTTON_VOLUME_STEP     10

static const char *TAG = "button";

typedef enum {
    BUTTON_WAKE = 0,
    BUTTON_VOL_DOWN,
    BUTTON_VOL_UP,
    BUTTON_COUNT
} button_id_t;

typedef struct {
    gpio_num_t gpio_num;
    const char *name;
} button_cfg_t;

typedef struct {
    int stable_level;
    int last_sampled_level;
    uint8_t debounce_count;
} button_state_t;

static const button_cfg_t s_button_cfg[BUTTON_COUNT] = {
    [BUTTON_WAKE] = { .gpio_num = WAKE_BUTTON_GPIO, .name = "wake" },
    [BUTTON_VOL_DOWN] = { .gpio_num = VOL_DOWN_BUTTON_GPIO, .name = "volume_down" },
    [BUTTON_VOL_UP] = { .gpio_num = VOL_UP_BUTTON_GPIO, .name = "volume_up" },
};

static button_state_t s_button_state[BUTTON_COUNT];

static bool button_is_pressed_level(int level)
{
    return level == BUTTON_ACTIVE_LEVEL;
}

/* 把按键动作集中放这里，主轮询逻辑会更清楚。 */
static void button_handle_press(button_id_t button_id)
{
    switch (button_id) {
    case BUTTON_WAKE:
        ESP_LOGI(TAG, "Wake button pressed");
        break;
    case BUTTON_VOL_DOWN:
        audio_out_volume_down(BUTTON_VOLUME_STEP);
        ESP_LOGI(TAG, "Volume down pressed, volume=%u%%", audio_out_get_volume_percent());
        break;
    case BUTTON_VOL_UP:
        audio_out_volume_up(BUTTON_VOLUME_STEP);
        ESP_LOGI(TAG, "Volume up pressed, volume=%u%%", audio_out_get_volume_percent());
        break;
    default:
        break;
    }
}

void button_init(void)
{
    /* 三个按键一次性完成 GPIO 配置。 */
    const uint64_t button_mask =
        (1ULL << WAKE_BUTTON_GPIO) |
        (1ULL << VOL_DOWN_BUTTON_GPIO) |
        (1ULL << VOL_UP_BUTTON_GPIO);

    const gpio_config_t io_conf = {
        .pin_bit_mask = button_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    for (size_t i = 0; i < BUTTON_COUNT; ++i) {
        /* 启动时先读取一次当前电平，作为去抖初始状态。 */
        const int level = gpio_get_level(s_button_cfg[i].gpio_num);
        s_button_state[i].stable_level = level;
        s_button_state[i].last_sampled_level = level;
        s_button_state[i].debounce_count = 0;
        ESP_LOGI(TAG, "Button %s ready on GPIO%d, idle_level=%d",
                 s_button_cfg[i].name,
                 s_button_cfg[i].gpio_num,
                 level);
    }
}

void button_task(void *pvParameters)
{
    (void)pvParameters;

    while (1) {
        for (size_t i = 0; i < BUTTON_COUNT; ++i) {
            button_state_t *state = &s_button_state[i];
            const int level = gpio_get_level(s_button_cfg[i].gpio_num);

            /* 连续多次采到相同电平，才认为状态真正稳定。 */
            if (level == state->last_sampled_level) {
                if (state->debounce_count < BUTTON_DEBOUNCE_COUNT) {
                    state->debounce_count++;
                }
            } else {
                state->last_sampled_level = level;
                state->debounce_count = 1;
            }

            if (state->debounce_count >= BUTTON_DEBOUNCE_COUNT &&
                level != state->stable_level) {
                state->stable_level = level;
                /* 只在按下沿触发，不在松开时重复触发。 */
                if (button_is_pressed_level(level)) {
                    button_handle_press((button_id_t)i);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}
