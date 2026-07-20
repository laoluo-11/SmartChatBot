/* =========================================================================
 * main.c —— 程序入口（第 5 关 L5：按键 + 状态机，把各模块串成"机器人"）
 * -------------------------------------------------------------------------
 * 现在各硬件模块各司其职：
 *   - mic.c           ：麦克风采集（打印 RMS 音量，做"系统还活着"的指示）
 *   - audio_out.c     ：喇叭（audio_out_play_tone 播测试音 / audio_out_task 自动嘀）
 *   - oled.c          ：OLED（oled_show_status 显示状态，oled_draw_text 写任意字）
 *   - led.c           ：板载 RGB（led_set_color 按状态变色）
 *   - state_machine.c ：状态机（中枢：切状态时自动驱动 OLED + LED + 喇叭）
 *   - button.c        ：按键（中断 → 队列 → 任务 → 回调，推进状态机）
 *
 * L5 的玩法：三个按键各管一件事（见 button.h 的接线宏）
 *   按键1 (GPIO0  唤醒)    -> 状态切到 LISTENING，开始一次聆听/对话
 *   按键2 (GPIO39 音量-)   -> 喇叭音量 -10，并在屏幕显示 VOL xx%
 *   按键3 (GPIO40 音量+)   -> 喇叭音量 +10，并在屏幕显示 VOL xx%
 *
 * 本文件把"所有模块的公开函数"都调用了一遍：
 *   - app_main 里完成 初始化 + 创建后台任务 + 开机自检演示
 *   - 开机自检(demo_all_features) 逐一调用每个模块尚未用到的函数，
 *     保证 mic / 喇叭 / OLED / LED / 状态机 / 按键 的全部能力都被用到。
 *
 * 给初学者的小知识：
 *   app_main() 是 ESP32 程序的总入口，相当于电脑程序的 main()。
 *   芯片上电、FreeRTOS 启动完后，会自动调用它。
 *   "任务(Task)"是 FreeRTOS 里可同时跑的多个小程序；xTaskCreate 把它们启动起来。
 * ========================================================================= */

#include <stdio.h>              // 标准输入输出（习惯带上）
#include "freertos/FreeRTOS.h" // FreeRTOS 内核
#include "freertos/task.h"     // 任务创建 xTaskCreate / 延时 vTaskDelay
#include "esp_log.h"           // 日志打印
#include "esp_heap_caps.h"     // 内存能力查询：探测板子有没有 PSRAM
#include "led.h"               // 板载 RGB 灯（led_init / led_set_color / led_off）
#include "mic.h"               // 麦克风模块（mic_init / mic_task）
#include "audio_out.h"         // 喇叭模块（audio_out_init / play_tone / task / 音量）
#include "oled.h"              // OLED 模块（oled_init / 清屏 / 画点 / 写字 / 显示）
#include "state_machine.h"     // 状态机（bot_init / bot_set_state / bot_get_state / bot_state_to_str）
#include "button.h"            // 按键模块（button_init / button_task / button_register_callback）

static const char *TAG = "main";  // 本文件日志标签："main: ..."（入口相关的日志归这里）

/* -------------------------------------------------------------------------
 * show_volume：把当前音量打到 OLED（音量键调完顺手看一下）
 * ------------------------------------------------------------------------- */
static void show_volume(void)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", audio_out_get_volume());
    oled_show_lines("VOL:", buf, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * on_button_pressed：按键"按下"时要做的事（按键模块会在任务里回调它）
 * 回调拿到的是"动作"（button_action_t），而不是 gpio 号，所以这里按动作分派：
 *   - 唤醒键  -> 进入 LISTENING（开始聆听；已在聆听则状态机忽略同状态）
 *   - 音量-   -> 音量降低，屏幕显示新音量
 *   - 音量+   -> 音量升高，屏幕显示新音量
 * 切状态的"变屏幕/变灯/出声"由 bot_set_state 自动完成；音量由 audio_out 模块处理。
 * ------------------------------------------------------------------------- */
static void on_button_pressed(button_action_t action)
{
    switch (action) {
        case BTN_WAKE:
            ESP_LOGI(TAG, "唤醒键：进入 LISTENING");
            bot_set_state(STATE_LISTENING);
            break;
        case BTN_VOL_DOWN:
            audio_out_volume_down();
            show_volume();
            break;
        case BTN_VOL_UP:
            audio_out_volume_up();
            show_volume();
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------------------
 * demo_all_features：开机自检——逐一调用各模块"还没被用到的"公开函数，
 * 让 mic / 喇叭 / OLED / LED / 状态机 的全部能力都被执行到一遍。
 * 这个函数只在开机时跑一次（演示完后状态机会自动回到 IDLE），不影响后面的交互。
 * ------------------------------------------------------------------------- */
static void demo_all_features(void)
{
    ESP_LOGI(TAG, "===== 开机自检：逐一调用各模块功能 =====");

    /* ---- 1) LED：亮红 → 亮绿 → 熄灭（直接调用 led_set_color / led_off） ---- */
    ESP_LOGI(TAG, "[LED] 亮红色");
    led_set_color(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP_LOGI(TAG, "[LED] 亮绿色");
    led_set_color(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP_LOGI(TAG, "[LED] 熄灭（led_off）");
    led_off();

    /* ---- 2) OLED：清屏 → 画点 → 写字 → 刷新 → 状态屏 → 多行（把 oled_xxx 都跑一遍） ---- */
    ESP_LOGI(TAG, "[OLED] 清屏 + 画像素点 + 写文字 + 刷新");
    oled_clear();                       // 清掉帧缓冲
    oled_draw_pixel(10, 10, true);      // 画几个白点（前景演示）
    oled_draw_pixel(12, 10, true);
    oled_draw_pixel(10, 12, true);
    oled_draw_text(0, 0, "HELLO");      // 用内置 8x8 字体写一行
    oled_draw_text(0, 16, "ESP32-S3");
    oled_refresh();                     // 把帧缓冲推到屏幕（必须调，否则不显示）
    vTaskDelay(pdMS_TO_TICKS(600));

    ESP_LOGI(TAG, "[OLED] oled_show_status(\"DEMO\")");
    oled_show_status("DEMO");           // 状态机也会调它，这里单独演示一次
    vTaskDelay(pdMS_TO_TICKS(600));

    ESP_LOGI(TAG, "[OLED] oled_show_lines(4 行)");
    oled_show_lines("LINE0", "LINE1", "LINE2", "LINE3");
    vTaskDelay(pdMS_TO_TICKS(600));

    /* ---- 3) 喇叭：设音量 + 播测试音（audio_out_set_volume / audio_out_play_tone） ---- */
    ESP_LOGI(TAG, "[AUDIO] 设置音量 80%%（audio_out_set_volume）");
    audio_out_set_volume(80);
    show_volume();                      // 顺手在屏幕上看一下当前音量
    vTaskDelay(pdMS_TO_TICKS(400));

    ESP_LOGI(TAG, "[AUDIO] 播放测试音 1kHz 500ms（audio_out_play_tone）");
    audio_out_play_tone(1000, 500);
    vTaskDelay(pdMS_TO_TICKS(400));

    /* ---- 4) 状态机：遍历所有状态，验证 LED/OLED/喇叭 自动联动 ---- */
    ESP_LOGI(TAG, "[STATE] 遍历状态机 IDLE->LISTEN->THINK->SPEAK->IDLE");
    bot_set_state(STATE_LISTENING);     // 蓝灯 + 屏显 LISTENING
    vTaskDelay(pdMS_TO_TICKS(600));
    bot_set_state(STATE_THINKING);      // 紫灯 + 屏显 THINKING
    vTaskDelay(pdMS_TO_TICKS(600));
    bot_set_state(STATE_SPEAKING);      // 绿灯 + 屏显 SPEAKING + 自动播 1kHz 测试音
    vTaskDelay(pdMS_TO_TICKS(1000));
    bot_set_state(STATE_IDLE);          // 回到空闲（灭灯、屏显 IDLE）

    /* ---- 5) 状态机查询接口（bot_get_state / bot_state_to_str） ---- */
    bot_state_t s = bot_get_state();
    const char *name = bot_state_to_str(s);
    ESP_LOGI(TAG, "[STATE] 当前状态=%s (%d)", name, s);

    ESP_LOGI(TAG, "===== 开机自检完成 =====");
}

/* -------------------------------------------------------------------------
 * app_main：程序入口
 * ------------------------------------------------------------------------- */
void app_main(void)
{
    /* 第一步：探测板子到底有没有 PSRAM（外部大内存）。
     * 你的板子是真 N16R8，所以这里会 > 0，走 else 分支。 */
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total == 0) {
        ESP_LOGI(TAG, "=== 语音聊天机器人 固件 (L5 按键+状态机) ===");
        ESP_LOGI(TAG, "[模式] 本板无 PSRAM，运行于内部 SRAM 模式。");
    } else {
        ESP_LOGI(TAG, "=== 语音聊天机器人 固件 (L5 按键+状态机) ===");
        ESP_LOGI(TAG, "PSRAM 已启用，总容量=%u KB", (unsigned)(psram_total / 1024));  // 字节转 KB 打印
    }

    /* 第二步：初始化所有硬件模块。
     * 注意顺序：led_init 必须先于 bot_init（状态机会在初始化时灭灯）。 */
    led_init();        // 初始化板载 RGB 灯（抽出 led_init，不再用心跳任务）
    oled_init();       // 建立 I2C 总线 + SSD1306 面板，清屏
    mic_init();        // 打开并配置 I2S 麦克风通道
    audio_out_init();  // 打开并配置 I2S 喇叭发送通道

    /* 第三步：初始化状态机（把当前状态设成 IDLE，并刷一次屏幕 + 灭灯） */
    bot_init();

    /* 第四步：注册"按键按下"的回调，并初始化按键（GPIO 中断 + 事件队列）。
     * 之后每次按键 → 进中断发信号 → button_task 消抖 → 调用 on_button_pressed。 */
    button_register_callback(on_button_pressed);
    button_init();

    /* 第五步：创建后台任务。
     * 麦克风持续采集（打印 RMS）作为"系统活着"的指示；
     * 按键任务等中断事件、消抖、分派动作；
     * audio_out_task 是可选演示：每 3 秒自动"嘀"一声，方便确认喇叭一直正常。 */
    xTaskCreate(mic_task, "mic", 4096, NULL, 4, NULL);    // 麦克风任务（栈 4KB 含缓冲区）
    xTaskCreate(button_task, "btn", 3072, NULL, 5, NULL); // 按键任务（栈 3KB 足够）
    xTaskCreate(audio_out_task, "spk_demo", 3072, NULL, 3, NULL); // 喇叭自动嘀演示任务

    /* 第六步：开机自检——逐一调用各模块尚未用到的函数，确保所有功能都被用到。
     * （放最后，避免自检的音/画面被后台任务打扰；自检结束后状态回到 IDLE。） */
    demo_all_features();

    ESP_LOGI(TAG, "系统就绪：按一下按键，状态机前进（IDLE→LISTEN→THINK→SPEAK→...）");
}
