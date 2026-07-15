/* =========================================================================
 * audio_out.c —— 喇叭(MAX98357A)播放模块的"具体实现"
 * -------------------------------------------------------------------------
 * 负责两件事：
 *   1) audio_out_init()      ：配置并启动 I2S 发送通道（相当于"打开喇叭"）
 *   2) audio_out_play_tone() ：生成一段正弦波并送进功放播放
 *
 * 给初学者的小知识：
 *   - MAX98357A 是"数字功放"：它直接吃 I2S 数字信号，内部自带 DAC，不用你外接 DAC 芯片。
 *   - 它和麦克风是"对称"的：麦克风用 I2S 接收(RX，数据从 DIN 进)；喇叭用 I2S 发送(TX，
 *     数据从 DOUT 出)。两者配置几乎一模一样，只是 RX↔din、TX↔dout，方向反过来。
 *   - 所谓"播放声音"，本质就是：在内存里造一串正弦波样本（int16 数组）→ 用 i2s_channel_write
 *     不断写给功放 → 功放把它变成模拟电压推动喇叭纸盆振动 → 发出声音。
 * ========================================================================= */

 #include "audio_out.h"
 #include <stdio.h>
 #include <math.h>
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 #include "esp_log.h"


/* 模块内部的"私有变量"：static 表示只在 audio_out.c 里可见，别的文件碰不到 */
static const char *TAG = "audio_out";         // 本模块日志标签："audio_out: ..."
static i2s_chan_handle_t  spk_tx_chan = NULL;   // I2S 发送通道句柄（喇叭的遥控器），先置空

/* 一个圆周率常量（自己定义，避免依赖编译器是否提供 M_PI） */
#define MY_PI   3.14159265f

/* -------------------------------------------------------------------------
 * audio_out_init：打开并配置喇叭（只调用一次）
 * 注意看：和 mic_init 几乎一样，区别只有两处：
 *   ① i2s_new_channel 第二参数是 TX 句柄（麦克风是第三参数 RX 句柄）
 *   ② .gpio_cfg 里用 .dout（数据输出到功放），.din 标记不用（麦克风相反）
 * ------------------------------------------------------------------------- */

void audio_out_init(void)
{

     /* 1) 通道配置：用 I2S1 控制器、ESP32 当主设备（主设备负责发时钟） */
     i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SPK_NUM,I2S_ROLE_MASTER);
 /* 2) 创建通道：只传 TX（发送）句柄，RX（接收）填 NULL → "只要发送，不要接收"。
     *    喇叭是纯输出设备，把声音送出去就行；收声音是麦克风的事。 */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg,&spk_tx_chan,NULL)); // 仅 TX
    
    
    /* 3) 配置 I2S 标准模式参数（时钟 / 声道 / 引脚）
     *    声道格式必须和麦克风一致（Philips / 16bit / MONO），否则功放不知道怎么解析数据。
     *    【若喇叭完全没声音】可尝试把下面这行 I2S_SLOT_MODE_MONO 改成 I2S_SLOT_MODE_STEREO
     *    再编译（极少数功放固件按立体声帧解析），其余代码不用动。 */
    i2s_std_config_t  std_cfg={
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_MONO), 
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,  // 主时钟 MCLK：MAX98357A 不需要，标记"不用"
            .bclk = SPK_BCLK_GPIO,    // 位时钟 → GPIO15（接功放 BCLK）
            .ws   = SPK_WS_GPIO,       // 声道选择 → GPIO16（接功功放 WSEL/LRC）
            .dout = SPK_DOUT_GPIO,     // 数据输出 → GPIO7（接功放 DIN，声音从这里发出去）
            .din  = I2S_GPIO_UNUSED,   // 数据输入：喇叭是输出设备，无输入，标记"不用"
            .invert_flags = {
                .bclk_inv = false,    // 不反转时钟极性（标准接法）
                .ws_inv   = false,     // 不反转声道极性
            }
        }
    };

     /* 4) 应用配置 + 启用通道。
     *    v6 的 i2s_channel_init_std_mode 不分 _rx/_tx，方向由前面 new_channel 传的句柄决定。 */
     ESP_ERROR_CHECK(i2s_channel_init_std_mode(spk_tx_chan,&std_cfg));
     ESP_ERROR_CHECK(i2s_channel_enable(spk_tx_chan));

     ESP_LOGI(TAG, "I2S 喇叭已启动: %d Hz, 16bit, MONO", SPK_SAMPLE_RATE);


}


void audio_out_play_tone(uint16_t  freq_hz,uint32_t  duration_ms)
{
    if(spk_tx_chan == NULL){
        ESP_LOGE(TAG,"喇叭未初始化，无法播放");  // 防御：喇叭没初始化就别播，打印错误直接返回
        return;
    }

    const int sr = SPK_SAMPLE_RATE;         // 采样率（每秒样本数）
    const int total = sr * duration_ms / 1000; // 这次总共要多少样本（如 16000×1s/1000 = 16000）
    int16_t *buf = (int16_t *)malloc(total * sizeof(int16_t)); // 申请缓冲区存正弦波（16000个样本≈32KB）
    if(!buf){
        ESP_LOGE(TAG,"音频缓冲malloc失败");
        return;
    }

    /* 生成正弦波：每个样本 = sin(2π·f·t) × 振幅。
     *   t = i / sr 表示第 i 个样本的时间点（秒）
     *   振幅用 30000（不是满量程 32767），留一点余量防止破音/削顶失真 */
     for(int i=0; i < total; i++){
        buf[i] = (int16_t)(sinf(2.0f * MY_PI * freq_hz * i / sr)*30000.0f);
     }
     /* 把缓冲区写给功放。一次可能写不完 32KB（I2S 内部 DMA 缓冲有限），
     * 所以用 while 循环、每次最多写 4KB，直到全部样本发完。 */
    size_t  offset = 0;                   // 已发送的样本数
    while (offset < (size_t) total)
    {
        size_t chunk = (total - offset) * sizeof(int16_t);  // 还剩多少字节没发
        if (chunk > 4096) chunk = 4096;                      // 单次最多发 4KB
        size_t written = 0;                                  // 本次实际写出的字节数（出参）
        esp_err_t r = i2s_channel_write(spk_tx_chan,
                                        buf + offset, 
                                        chunk,
                                        &written,
                                        pdMS_TO_TICKS(1000));
        if (r != ESP_OK) {   // 写出错则放弃，避免死循环
            ESP_LOGE(TAG, "i2s 写失败: %d", r);
            break;
        }
        offset += written / sizeof(int16_t);   // 推进已发送样本数
    }
    free(buf);  // 用完释放内存，养成好习惯
    ESP_LOGI(TAG, "播放测试音 %u Hz, %u ms 完成", freq_hz, duration_ms);
    
}

/* -------------------------------------------------------------------------
 * audio_out_task：播放任务（被 FreeRTOS 反复调度，无限循环）
 * 设计：播 1 秒 1kHz 测试音 → 停 2 秒 → 再播，循环往复。
 *   这样你不用按键就能反复听到喇叭出声，便于确认 L3 跑通。
 * ------------------------------------------------------------------------- */

void audio_out_task(void *pvParaments)
{
    (void)pvParaments;  // 本任务不接收参数
    /* 给 I2S/DMA 一点启动时间，避免第一声被截断 */
    vTaskDelay(pdMS_TO_TICKS(500));
    while (1) {
        audio_out_play_tone(1000,1000);      // 播放 1kHz 正弦波 1 秒
        vTaskDelay(pdMS_TO_TICKS(2000));    // 静音 2 秒，循环
    }
}
