/* =========================================================================
 * mic.c —— 麦克风(INMP441)采集模块的"具体实现"
 * -------------------------------------------------------------------------
 * 负责两件事：
 *   1) mic_init()  ：配置并启动 I2S 接收通道（相当于"打开麦克风"）
 *   2) mic_task()  ：不停地读声音数据，算出一个"音量大小"(RMS) 并打印
 *
 * 给初学者的小知识：
 *   - INMP441 是"数字麦克风"，用 I2S 这种高速数字总线传声音，有 3 根关键线：
 *       BCLK（节拍时钟）、WS（左右声道选择）、DIN（数据，ESP32 接收）。
 *     ESP32 是"主"，负责发时钟，麦克风跟着走。
 *   - RMS（均方根）是衡量"音量大小"的常用指标：把每个样本平方→求平均→开根号。
 * ========================================================================= */

#include "mic.h"               // 本模块声明（引脚宏、mic_init/mic_task 原型）
#include <stdio.h>            // 标准输入输出（习惯带上）
#include <stdlib.h>           // malloc / free
#include <math.h>             // 数学库，用 sqrtf() 开平方根算 RMS
#include "freertos/FreeRTOS.h"// FreeRTOS 内核
#include "freertos/task.h"    // 任务函数（xTaskCreate / vTaskDelete / vTaskDelay）
#include "esp_log.h"          // 日志打印 ESP_LOGI / ESP_LOGE
#include "esp_heap_caps.h"    // 内存能力分配（采集缓冲放 PSRAM）
#include "state_machine.h"    // L8：bot_get_state() 判断当前状态，决定是否喂唤醒词
#include "wake.h"             // L8：wake_feed() 离线唤醒词检测
#include "comm.h"             // L7：流式上传（comm_send_audio / comm_is_connected）
#include "opus_codec.h"       // L7：opus_encode_frame（边录边编码）

/* 模块内部的"私有变量"：static 表示只在 mic.c 里可见，别的文件碰不到。
 * 这样可以避免不同模块之间变量名互相打架。 */
static const char *TAG = "mic";                 // 本模块日志标签："mic: ..."
static i2s_chan_handle_t mic_rx_chan = NULL;    // I2S 接收通道句柄（想象成麦克风的遥控器），先置空
static volatile float g_mic_rms = 0.0f;          // 最近一次 RMS 值（状态机读取用，volatile 防跨任务优化）

/* ---- L7 流式上传：LISTENING 期间"边录边编码边发"，不再等整句录完再批量上传 ----
 * 每凑满一帧（OPUS_FRAME_SAMPLES = 960 样本 @16k = 60ms）就 Opus 编码并立即发一帧，
 * VAD 静音截止由 state_machine.c 发 audio_end。首字延迟只取决于网络，不再受 8 秒缓冲限制。 */

/* 软件麦克风增益：INMP441 经 32bit 读 + >>16 取出的是标准 16bit PCM（满量程 ±32768，
 * 正常说话 RMS 几百~一两千），本就清晰可辨。这里再乘 MIC_GAIN 适度放大以补偿灵敏度。
 * 【注意】增益别贪大：×6 时麦本底噪声(RMS 6~13)被放大成明显"沙沙电流声"，
 * 且说话峰值撞上软限幅产生削波 →"发闷/机器人"失真。×2 是安全值；
 * 若回声音量偏小优先调播放音量(g_volume)，而不是加这里。 */
#define MIC_GAIN  2

/* ---- 单极点高通(去直流 + 滤低频哼声) ----
 * 公式 y[n] = x[n] - x[n-1] + A*y[n-1]，固定系数 A=97/100≈0.97 → 截止频率 ≈76Hz。
 * 作用：剔除电源/接地引入的直流偏置和 50/60Hz 工频"嗡嗡"声。MAX98357A 的 SD 脚被硬
 *       接 3.3V（功放常通），一旦麦吃到的直流/哼声被录进去并回声，D 类功放会把它放大成
 *       电流声。高通对语音(基频≥85Hz、靠谐波可懂)几乎无损，但能显著压低低频嗡声。
 * 注意：这只解决"麦吃到的低频嗡声"。若嗡声来自功放自身供电(播放静音也有)，需改硬件。 */
static int32_t s_hp_in  = 0;    // 上一拍输入（做差分去直流）
static int32_t s_hp_out = 0;    // 上一拍输出（一极点状态）
#define HP_A_NUM  97            // 高通系数分子（分母 HP_A_DEN）
#define HP_A_DEN  100

/* 软限幅：把超出 ±thresh 的部分压缩（只收 1/4 溢出），避免硬削波到 32767
 * 产生"闷/破音"。普通说话（峰值 < thresh）完全不受影响，只有很大声才压缩。 */
static int32_t mic_soft_clip(int32_t s, int32_t thresh)
{
    if (s >  thresh) return  thresh + (s -  thresh) / 4;
    if (s < -thresh) return -thresh + (s +  thresh) / 4;
    return s;
}

static bool     g_stream_upload = false;   // LISTENING 期间为真：mic_task 每满一帧就编码上传
static int16_t  s_frame_buf[OPUS_FRAME_SAMPLES]; // Opus 帧累积缓冲（960 样本，约 1.9KB）
static int      s_frame_len = 0;          // 已累积的样本数

/* -------------------------------------------------------------------------
 * mic_init：打开并配置麦克风（只调用一次）
 * ------------------------------------------------------------------------- */
void mic_init(void)
{
    if (mic_rx_chan != NULL) {
        ESP_LOGW(TAG, "麦克风已初始化，跳过重复初始化");
        return;
    }

    /* 1) 通道配置：用哪个 I2S 控制器、ESP32 当主设备 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_NUM, I2S_ROLE_MASTER);

    /* 2) 创建通道：只传 RX（接收）句柄，TX（发送）填 NULL → "只要接收，不要发送"。
     *    麦克风只负责把声音送进来；往外发声音是后面 L3 喇叭的事。 */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &mic_rx_chan)); // 仅 RX

    /* 3) 配置 I2S 的"标准模式"参数（时钟 / 声道 / 引脚） */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),   // 时钟：用上面定义的 16000Hz
        /* INMP441 是 24bit 数字麦，I2S 上【左对齐】传输：24bit 数据放在 32bit 帧的高 24 位
         * （bit 31..8），低 8 位恒为 0。
         *   - 用 16bit 槽读 → 取到低 16 位（噪声地板）→ 失真、只剩一点音调（已验证）；
         *   - 用 32bit 槽读 → raw = audio24 << 8，取满幅 16bit 真值需右移 16：
         *       16bit 音频样本 = (int32_t)raw >> 16
         *
         * 【关键教训：为什么必须 STEREO 双槽读】
         * ESP32-S3 的 I2S RX 配 MONO 时，硬件帧结构实际仍是"左槽+右槽"双槽，
         * 两个槽的数据都会进 DMA！INMP441(L/R=GND) 只在左槽发声，右槽是悬空
         * 数据线的"残影"。于是录音变成 [真语音, 幽灵, 真语音, 幽灵, ...] 交替
         * —— 隔样本污染 = 环形调制 = "汽车人/金属声"。
         * （WAV 法医分析实锤：偶样本 RMS≈800/自相关 0.93，奇样本 RMS≈165；
         *   全序列 lag1 自相关 -0.09；8kHz 奈奎斯特附近能量异常强。）
         * 修复：显式配 STEREO 双槽（这也让 SCK=64×WS，正是 INMP441 数据手册要求），
         * 软件里每帧只取左槽（偶数下标），得到干净的 16kHz 单声道。 */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // 主时钟 MCLK：INMP441 不需要，标记"不用"
            .bclk = MIC_BCLK_GPIO,     // 位时钟 → GPIO5（接麦克风 SCK）
            .ws   = MIC_WS_GPIO,       // 声道选择 → GPIO4（接麦克风 WS）
            .dout = I2S_GPIO_UNUSED,   // 数据输出：麦克风是输入设备，无输出，标记"不用"
            .din  = MIC_DIN_GPIO,      // 数据输入 → GPIO6（接麦克风 SD，声音从这里进来）
            .invert_flags = {
                .bclk_inv = false,     // 不反转时钟极性（标准接法保持默认）
                .ws_inv   = false,     // 不反转声道极性
            },
        },
    };

    /* 4) 把配置应用到通道。
     * 【初学者注意】ESP-IDF v6 把原来的 i2s_channel_init_std_rx / _tx 合并成
     *    i2s_channel_init_std_mode()，方向(RX/TX)由前面 i2s_new_channel 传的句柄决定，
     *    不再靠函数名区分。所以这里不写 _rx，直接写 _mode。 */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_rx_chan));  // 5) 启用通道：正式开始"听"

    ESP_LOGI(TAG, "I2S 麦克风已启动: %d Hz, 32bit槽 STEREO 双槽读(只取左槽,>>16)", SAMPLE_RATE);  // 打印确认信息
}

/* -------------------------------------------------------------------------
 * mic_task：采集任务（被 FreeRTOS 反复调度，无限循环）
 * ------------------------------------------------------------------------- */
void mic_task(void *pvParameters)
{
    (void)pvParameters;   // 本任务不接收外部参数，消除"未使用参数"警告

    /* 申请内存当"缓冲区"：STEREO 双槽读，每个"音频样本"占 2 个 int32（左槽+右槽），
     * 所以 buf32 要按 MIC_READ_SAMPLES*2 申请（512 样本 → 4KB）。
     * 另开一块 int16 缓冲，存"只取左槽 + >>16"转换后的单声道样本，
     * 供 RMS / 唤醒词检测 / 采集缓冲复用，避免重复转换。 */
    int32_t *buf32 = (int32_t *)malloc(MIC_READ_SAMPLES * 2 * sizeof(int32_t));
    int16_t *s16buf = (int16_t *)malloc(MIC_READ_SAMPLES * sizeof(int16_t));
    if (!buf32 || !s16buf) {   // 万一内存申请失败（返回 NULL），兜底退出，不能让程序崩
        ESP_LOGE(TAG, "麦克风缓冲 malloc 失败，任务退出");
        vTaskDelete(NULL);  // 删除自己这个任务，安全退出
        return;
    }

    size_t bytes_read = 0;   // 记录"这次实际读到了多少字节"
    uint32_t cnt = 0;        // 计数器：控制"大约每 1 秒打印一次"，避免刷屏

    /* 主循环：不停地读麦克风 */
    while (1) {
        /* i2s_channel_read：从麦克风通道读数据到 buf32。
         * STEREO 双槽：DMA 数据是 [左槽, 右槽, 左槽, 右槽, ...] 交替，
         * 每个音频采样周期占 8 字节（2 个 int32）。
         * 参数：通道句柄、目标缓冲、想读的最大字节数、实际读到的字节数(出参)、超时(1秒) */
        esp_err_t r = i2s_channel_read(mic_rx_chan, buf32,
                                       MIC_READ_SAMPLES * 2 * sizeof(int32_t),
                                       &bytes_read, pdMS_TO_TICKS(1000));
        if (r == ESP_OK && bytes_read > 0) {   // 读成功且确实读到了数据
            /* 每个"音频样本" = 左槽+右槽 2 个 int32 = 8 字节 */
            int samples = (int)(bytes_read / (2 * sizeof(int32_t)));

            /* --- 只取左槽 + 32bit→16bit 还原 + 计算 RMS ---
             * INMP441(L/R=GND) 的数据在【左槽】=偶数下标；右槽是悬空线残影，必须丢弃
             * （之前 MONO 配置把右槽残影也当样本收进来 → 隔样本污染 → "汽车人声"）。
             * 24bit 真值左对齐在 32bit 高 24 位，故 16bit 真值 = raw >> 16。 */
            int64_t sum = 0;                    // 用 int64 累加，防止平方和溢出
            for (int i = 0; i < samples; i++) {
                int32_t x = (int32_t)(buf32[i * 2] >> 16);  // i*2 = 左槽；>>16 取高16位真值
                /* 单极点高通：去直流偏置 + 滤掉 50/60Hz 工频哼声（A=97/100≈0.97 → 截止~76Hz）。
                 * 同帧既喂唤醒词检测、RMS，也喂采集缓冲，一处去直流即可。 */
                int32_t y = x - s_hp_in + (HP_A_NUM * s_hp_out) / HP_A_DEN;
                s_hp_in  = x;
                s_hp_out = y;
                /* 高通在高频瞬态处增益可接近 2 倍，y 可能略超 int16 范围；
                 * 必须钳位后再转 int16，否则回绕成反相大值 → "咔哒"爆点。 */
                if (y >  32767) y =  32767;
                if (y < -32768) y = -32768;
                int16_t v = (int16_t)y;         // 去直流后的干净样本
                s16buf[i] = v;
                int32_t s = v;                 // 取第 i 个样本（16bit 有符号，范围约 -32768~32767）
                sum += (int64_t)s * s;          // 平方后累加
            }
            float rms = sqrtf((float)((double)sum / samples));  // 平均后再开根号 = RMS
            g_mic_rms = rms;  // 更新全局 RMS 供状态机读取

            /* ---- L7：流式上传 ----
             * LISTENING 且已连服务器时，把这段 PCM 边录边编码：每凑满一帧(960 样本)就 Opus
             * 编码并立即发一帧。套用 MIC_GAIN + 软限幅，逻辑同原采集缓冲。
             * 这样上传不再受 8 秒缓冲上限限制（之前"最多 7 秒"正是它造成的）。 */
            if (g_stream_upload && comm_is_connected()) {
                for (int i = 0; i < samples; i++) {
                    int32_t s = (int32_t)s16buf[i] * MIC_GAIN;
                    s = mic_soft_clip(s, 24000);   // 软限幅，防硬削波
                    s_frame_buf[s_frame_len++] = (int16_t)s;
                    if (s_frame_len >= OPUS_FRAME_SAMPLES) {
                        uint8_t enc[OPUS_FRAME_SAMPLES * 2 + 64];
                        int n = opus_encode_frame(s_frame_buf, enc, (int)sizeof(enc));
                        if (n > 0) {
                            comm_send_audio(USE_OPUS ? CODEC_OPUS : CODEC_PCM,
                                            enc, (size_t)n);
                        }
                        s_frame_len = 0;
                    }
                }
            }

            /* ---- L8：离线唤醒词检测 ----
             * 仅在 IDLE 状态监听唤醒词（LISTENING/THINKING 期间由 VAD 控制，不必再唤醒；
             * SPEAKING 期间的自唤醒问题靠"只在 IDLE 响应"天然规避大部分，AEC 后续再加）。
             * 检测到唤醒词 → 进入 LISTENING，开始一轮对话（和按唤醒键效果相同）。 */
            if (bot_get_state() == STATE_IDLE) {
                if (wake_feed(s16buf, samples)) {
                    ESP_LOGI(TAG, "WAKENET: 检测到唤醒词 → 进入 LISTENING");
                    bot_set_state(STATE_LISTENING);
                }
            }

            /* --- 大约每 1 秒打印一次音量仪表（即使安静也打印，方便你观察对比）---
             * 512 样本 @16kHz 只够 32 毫秒，循环很快；数 32 次循环（≈1秒）才打一行，不刷屏。
             *
             * 三种状态对照（接不接麦、说没说话，看 RMS 一眼分辨）：
             *   悬空(没接麦)  → RMS 很高且乱跳（几千），一直"有声音"
             *   接麦且安静     → RMS 接近 0，显示"安静/基线"
             *   接麦且说话     → RMS 明显升高，显示"有声音"
             * 注意：下面 200 只是用来贴"有声音"标签，不是判断接没接麦的标准。 */
            if ((++cnt % 32) == 0) {   // ++cnt 先自增，再对 32 取余；余 0 说明凑满 32 次
                const char *state = (rms > 80.0f) ? "有声音" : "安静/基线";  // 三目运算贴标签（阈值随电平下调）
                ESP_LOGI(TAG, "mic RMS=%5.0f | %s", rms, state);  // %5.0f = 留 5 位宽、不要小数
            }
        }
    }
}

float mic_get_rms(void)
{
    return g_mic_rms;
}

/* ---- L7 流式上传接口实现 ----
 * mic_capture_start()/stop() 现在控制的是"是否边录边上传"，不再分配大缓冲。 */
void mic_capture_start(void)
{
    s_frame_len = 0;                 // 帧累积器清零
    g_stream_upload = true;          // 打开流式上传（mic_task 每满一帧就发）
    ESP_LOGI(TAG, "开始流式上传（边录边发，无时长上限）");
}

void mic_capture_stop(void)
{
    /* 停止前把最后一小段（不足一帧）补零到整帧后编码发出，避免丢尾。
     * 若根本没连服务器，直接放弃本轮音频。 */
    if (g_stream_upload && s_frame_len > 0 && comm_is_connected()) {
        while (s_frame_len < OPUS_FRAME_SAMPLES) s_frame_buf[s_frame_len++] = 0;
        uint8_t enc[OPUS_FRAME_SAMPLES * 2 + 64];
        int n = opus_encode_frame(s_frame_buf, enc, (int)sizeof(enc));
        if (n > 0) comm_send_audio(USE_OPUS ? CODEC_OPUS : CODEC_PCM, enc, (size_t)n);
        s_frame_len = 0;
    }
    g_stream_upload = false;
    ESP_LOGI(TAG, "流式上传停止");
}

const int16_t *mic_capture_get(size_t *samples)
{
    /* 兼容旧接口：流式模式下没有整段缓冲可取，返回 NULL（state_machine 不再依赖它）。 */
    *samples = 0;
    return NULL;
}
