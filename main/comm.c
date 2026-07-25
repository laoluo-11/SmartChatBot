/* =========================================================================
 * comm.c —— WebSocket 通信模块的"具体实现"（L7 新增）
 * -------------------------------------------------------------------------
 * 三块功能：
 *   1) 连接管理：esp_websocket_client 连服务器，连上/断开用事件回调
 *   2) 上行发送：comm_send_audio_frames() 切片+编码+逐帧发；comm_send_json() 发控制
 *   3) 下行接收：收到服务器二进制帧 → 入队；播放任务出队 → 解码 → 喇叭播放
 *
 * 给初学者的小知识：
 *   - esp_websocket_client 帮我们管 TCP/WebSocket 握手、自动重连，只需给 uri。
 *   - "收到数据"是异步事件（ws_event_handler 里），不能在那里做长时间播放，
 *     所以只把音频帧丢进队列，真正的播放在独立的 playback_task 里做。
 *   - 二进制帧第 1 字节是 codec 标识，后面才是音频负载（见 opus_codec.h）。
 * ========================================================================= */

#include "comm.h"
#include "opus_codec.h"      // 编解码（发送前编码、接收后解码）
#include "audio_out.h"       // audio_out_stream_begin / play_pcm / stream_end
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "comm";

/* ---- 模块内部状态 ---- */
static esp_websocket_client_handle_t s_client = NULL;
static comm_ctrl_cb_t      s_ctrl_cb   = NULL;   // 服务器 JSON 控制帧回调
static comm_connected_cb_t s_conn_cb   = NULL;   // 连上服务器回调
static comm_play_start_cb_t s_play_start_cb = NULL;
static comm_play_end_cb_t   s_play_end_cb   = NULL;
static bool s_connected = false;

/* ---- 接收音频帧队列 + 播放任务 ---- */
typedef struct {
    uint8_t *data;     // 音频负载（已去掉 codec 字节）
    size_t   len;
    uint8_t  codec;    // CODEC_OPUS / CODEC_PCM
} audio_frame_t;

static QueueHandle_t s_play_q = NULL;
static bool s_audio_ended = false;     // 服务器已发完这轮音频

/* ---- WebSocket 二进制消息"分片重组"缓冲 ----
 * 【关键 bug 修复】esp_websocket_client 的接收缓冲默认 1024 字节，而我们一帧
 * PCM 消息 = 1 字节 codec + 1920 字节音频 = 1921 字节 → 一条消息会拆成
 * 多次 WEBSOCKET_EVENT_DATA 事件（用 payload_offset / payload_len 标进度）。
 * 之前的代码把"每一片"都当成完整帧、都剥第 1 字节当 codec：
 *   - 第 1 片剥的确实是 codec，但剩 1023 字节（奇数）→ 尾部丢 1 字节；
 *   - 第 2 片剥掉的是"音频数据"→ 整片 PCM 错位 1 字节（高低字节互换）！
 * 于是每 60ms 音频 = 前一半正常 + 后一半字节错位的金属杂讯，周期性交替
 * → 这就是"汽车人/金属机器音"的元凶。必须按 payload_offset 重组整条消息。 */
static uint8_t *s_rx_asm     = NULL;   // 重组缓冲（存 codec 之后的音频负载）
static size_t   s_rx_asm_cap = 0;      // 缓冲容量
static uint8_t  s_rx_codec   = 0;      // 本条消息的 codec（第 1 片的第 1 字节）
static bool     s_rx_valid   = false;  // 本条消息是否有效（分配失败等则整条丢弃）

#define PLAY_TASK_STACK  10240
#define PLAY_Q_LEN      48

/* 预缓冲帧数：收到第一帧后不立刻开播，先等队列攒到这么多帧（每帧 960样本@16k=60ms，
 * 5 帧 ≈ 300ms）再开始播放。这样 WiFi 抖动时 DMA 不容易被掏空（欠载），消除流式
 * 播放的"断续/机器人抖动感"。若服务器提前发完(audio_end)则不足该数也立即开播。 */
#define PLAY_PREBUF_FRAMES  5
#define PLAY_PREBUF_WAIT_MS 600   /* 最多等这么久，防止服务器慢导致迟迟不出声 */

/* =========================================================================
 * 事件处理：连上 / 断开 / 收到数据 都是"异步事件"
 * ========================================================================= */
static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t id, void *event_data)
{
    (void)handler_args;

    if (base != WEBSOCKET_EVENTS) return;

    if (id == WEBSOCKET_EVENT_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "WebSocket 已连上服务器");
        if (s_conn_cb) s_conn_cb();

    } else if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "WebSocket 已断开（会自动重连）");

    } else if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

        /* 每个 DATA 事件携带本帧数据：data_ptr 指向数据、data_len 是长度。
         * 我们的协议每帧单独成一条 WebSocket 消息（不分片），直接用即可。 */
        if (d->data_len <= 0) return;
        const uint8_t *buf = (const uint8_t *)d->data_ptr;
        int len = d->data_len;

        if (d->op_code == WS_TRANSPORT_OPCODES_BINARY) {
            /* 二进制消息 = [1字节 codec] + 音频负载。
             * 一条消息可能分多片到达（见 s_rx_asm 注释），必须按 payload_offset
             * 重组，只有"第 1 片的第 1 字节"才是 codec，其余全是音频数据！ */
            size_t total_payload = (size_t)d->payload_len;   // 整条消息总长（含 codec）
            if (total_payload <= 1) return;
            size_t audio_total = total_payload - 1;          // 音频负载总长

            if (d->payload_offset == 0) {
                /* 新消息的第 1 片：取 codec，准备重组缓冲 */
                s_rx_codec = buf[0];
                s_rx_valid = false;
                if (audio_total > s_rx_asm_cap) {            // 缓冲不够就换大的（复用，不频繁 malloc）
                    free(s_rx_asm);
                    s_rx_asm = (uint8_t *)malloc(audio_total);
                    s_rx_asm_cap = s_rx_asm ? audio_total : 0;
                }
                if (!s_rx_asm) return;                       // 内存不足，整条消息丢弃
                s_rx_valid = true;
                memcpy(s_rx_asm, buf + 1, (size_t)len - 1);  // 去掉 codec 后拷入
            } else {
                /* 后续分片：整片都是音频数据，按偏移续写（-1 是扣掉 codec 字节） */
                if (!s_rx_valid) return;
                size_t off = (size_t)d->payload_offset - 1;
                if (off + (size_t)len > s_rx_asm_cap) { s_rx_valid = false; return; }  // 越界保护
                memcpy(s_rx_asm + off, buf, (size_t)len);
            }

            /* 整条消息收齐了才入队（最后一片：offset + len == payload_len） */
            if ((size_t)d->payload_offset + (size_t)len < total_payload) return;
            if (!s_rx_valid) return;

            uint8_t *copy = (uint8_t *)malloc(audio_total);
            if (!copy) return;
            memcpy(copy, s_rx_asm, audio_total);
            audio_frame_t f = { .data = copy, .len = audio_total, .codec = s_rx_codec };
            /* 队列满就丢最旧的一帧，避免播放卡死（宁可少一段，不要卡住） */
            if (xQueueSend(s_play_q, &f, 0) != pdTRUE) {
                audio_frame_t drop;
                if (xQueueReceive(s_play_q, &drop, 0) == pdTRUE) free(drop.data);
                xQueueSend(s_play_q, &f, 0);
            }
        } else if (d->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            /* 文本帧 = JSON 控制消息 */
            if (s_ctrl_cb) s_ctrl_cb((const char *)buf);
        }
    }
}

/* =========================================================================
 * 初始化
 * ========================================================================= */
esp_err_t comm_init(const char *uri, comm_ctrl_cb_t ctrl_cb)
{
    s_ctrl_cb = ctrl_cb;
    s_play_q  = xQueueCreate(PLAY_Q_LEN, sizeof(audio_frame_t));
    if (!s_play_q) {
        ESP_LOGE(TAG, "播放队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms   = 4000,
        .task_stack             = 10240,   /* 显式加大 websocket 内部任务栈，避免 recv 时栈溢出破坏 client->lock */
        .buffer_size            = 4096,    /* 【关键】默认 1024 < 一帧 PCM 1921 字节 → 消息被分片，
                                            * 旧代码不重组导致 PCM 错位 1 字节（"汽车人"金属声）。
                                            * 加到 4096 让整帧一次到齐；分片重组逻辑仍保留作兜底。 */
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "WebSocket 客户端创建失败");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    ESP_LOGI(TAG, "WebSocket 客户端已初始化，目标 %s", uri);
    return ESP_OK;
}

void comm_register_connected_cb(comm_connected_cb_t cb) { s_conn_cb = cb; }

void comm_register_playback_cbs(comm_play_start_cb_t start, comm_play_end_cb_t end)
{
    s_play_start_cb = start;
    s_play_end_cb   = end;
}

esp_err_t comm_connect(void)
{
    if (!s_client) return ESP_FAIL;
    ESP_LOGI(TAG, "正在连接服务器...");
    return esp_websocket_client_start(s_client);
}

bool comm_is_connected(void) { return s_connected; }

/* =========================================================================
 * 上行发送
 * ========================================================================= */
esp_err_t comm_send_json(const char *json)
{
    if (!s_connected || !s_client) return ESP_FAIL;
    int r = esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(2000));
    return (r > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t comm_send_audio(uint8_t codec, const uint8_t *data, size_t len)
{
    if (!s_connected || !s_client) return ESP_FAIL;
    /* 帧格式：[1 字节 codec] + 音频负载 */
    uint8_t *buf = (uint8_t *)malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = codec;
    memcpy(buf + 1, data, len);
    int r = esp_websocket_client_send_bin(s_client, (const char *)buf, len + 1, pdMS_TO_TICKS(2000));
    free(buf);
    return (r > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t comm_send_audio_frames(const int16_t *pcm, size_t samples)
{
    if (!s_connected) {
        ESP_LOGW(TAG, "未连服务器，跳过音频上传");
        return ESP_FAIL;
    }
    if (samples < (size_t)OPUS_FRAME_SAMPLES) {
        ESP_LOGW(TAG, "音频太短(%u 样本)，不足一帧，跳过", (unsigned)samples);
        return ESP_FAIL;
    }

    uint8_t *tmp = (uint8_t *)malloc(OPUS_FRAME_SAMPLES * 2 + 64);
    if (!tmp) return ESP_ERR_NO_MEM;

    uint8_t codec = (USE_OPUS ? CODEC_OPUS : CODEC_PCM);
    size_t sent = 0;
    int frames = 0;
    while (sent + OPUS_FRAME_SAMPLES <= samples) {
        int n = opus_encode_frame(pcm + sent, tmp, OPUS_FRAME_SAMPLES * 2 + 64);
        if (n > 0) {
            comm_send_audio(codec, tmp, (size_t)n);
            frames++;
        }
        sent += OPUS_FRAME_SAMPLES;
        vTaskDelay(pdMS_TO_TICKS(1));   /* 每帧让出 CPU，避免长循环饿死 websocket/播放任务、降低栈压力 */
    }
    free(tmp);
    ESP_LOGI(TAG, "已上传 %d 个音频帧（%u 样本）", frames, (unsigned)sent);
    return ESP_OK;
}

/* =========================================================================
 * 接收侧的"解码 + 播放"任务
 * ========================================================================= */
static void playback_task(void *arg)
{
    (void)arg;
    audio_frame_t f;
    int16_t *pcm = (int16_t *)malloc(OPUS_FRAME_SAMPLES * sizeof(int16_t) + 64);
    bool playing = false;

    while (1) {
        if (xQueueReceive(s_play_q, &f, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (!playing) {                       // 这一轮音频的第一帧
                /* 预缓冲：先别急着开播。等队列再攒 PLAY_PREBUF_FRAMES 帧（≈300ms 余量），
                 * 或等到 audio_end / 超时。否则服务器逐帧"贴着实时"发送时，
                 * WiFi 稍一抖 DMA 就欠载 → 断续机器人声。 */
                int waited = 0;
                while (uxQueueMessagesWaiting(s_play_q) < PLAY_PREBUF_FRAMES - 1 &&
                       !s_audio_ended && waited < PLAY_PREBUF_WAIT_MS) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    waited += 20;
                }
                playing = true;
                audio_out_stream_begin();          // 打开喇叭 I2S 通道
                if (s_play_start_cb) s_play_start_cb();
            }
            int n = opus_decode_packet(f.data, (int)f.len, pcm,
                                       OPUS_FRAME_SAMPLES * (int)sizeof(int16_t) + 64);
            if (n > 0) audio_out_play_pcm(pcm, (size_t)n);
            free(f.data);

            /* 队列空了且服务器已宣布发完 → 这轮播放结束 */
            if (uxQueueMessagesWaiting(s_play_q) == 0 && s_audio_ended) {
                playing = false;
                s_audio_ended = false;
                audio_out_stream_end();            // 关闭喇叭 I2S 通道
                if (s_play_end_cb) s_play_end_cb();
            }
        } else {
            /* 500ms 没新帧：若已宣布结束且队列空，收尾 */
            if (playing && uxQueueMessagesWaiting(s_play_q) == 0 && s_audio_ended) {
                playing = false;
                s_audio_ended = false;
                audio_out_stream_end();
                if (s_play_end_cb) s_play_end_cb();
            }
        }
    }
}

esp_err_t comm_start_playback_task(void)
{
    BaseType_t ok = xTaskCreate(playback_task, "play", PLAY_TASK_STACK, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "播放任务创建失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void comm_mark_audio_end(void)
{
    s_audio_ended = true;
    ESP_LOGI(TAG, "服务器音频流结束标记");
}

/* =========================================================================
 * 极简 JSON 取值：找 "key":"value"，拷 value（去引号）到 out
 * 仅支持 "key" 后紧跟 ":" 再紧跟 "value"（服务器按此格式发即可）。
 * ========================================================================= */
int comm_json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return -1;

    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += n;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;                                   // 跳过 ':'
    while (*p == ' ' || *p == '"') p++;    // 跳过空格和开头的引号
    const char *end = strchr(p, '"');      // value 到下一个引号结束
    if (!end) return -1;

    size_t len = (size_t)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}
