/* =========================================================================
 * comm.h —— WebSocket 通信模块（L7 新增）
 * -------------------------------------------------------------------------
 * 负责和"服务器端"建立长连接，按架构文档定的协议收发音/文：
 *
 *   ┌─ 上行（设备→服务器）─────────────────────────────────────────┐
 *   │ 控制消息：WebSocket 文本帧，JSON。如 {"type":"audio_end"}      │
 *   │ 音频消息：WebSocket 二进制帧 = [1字节 codec] + 音频负载          │
 *   │           codec=0x01 Opus / 0x02 PCM（见 opus_codec.h）          │
 *   └──────────────────────────────────────────────────────────────┘
 *   ┌─ 下行（服务器→设备）─────────────────────────────────────────┐
 *   │ 控制消息：文本帧 JSON（transcript / reply / audio_end ...）     │
 *   │ 音频消息：二进制帧 [codec] + 负载 → 内部解码 → 喇叭播放          │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 模块内部维护一条"接收音频帧队列 + 播放任务"：收到服务器音频后自动解码、
 * 连续播放，并在"开始播放 / 播放结束"时回调上层（上层据此切状态机）。
 * 采集侧（mic→Opus→发送）由状态机调用 comm_send_audio_frames() 触发。
 *
 * 依赖：esp_websocket_client（核心组件，已在 CMakeLists 的 REQUIRES 里）。
 * ========================================================================= */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* 服务器发来的 JSON 控制文本帧回调（如 transcript/reply/audio_end） */
typedef void (*comm_ctrl_cb_t)(const char *json);

/* "连上服务器"回调（可选，传 NULL 取消） */
typedef void (*comm_connected_cb_t)(void);

/* 播放回调：开始播放服务器音频 / 播放结束（上层据此切状态机 SPEAKING↔IDLE） */
typedef void (*comm_play_start_cb_t)(void);
typedef void (*comm_play_end_cb_t)(void);

/* 初始化 WebSocket 客户端。
 *   uri     ：服务器地址，形如 ws://192.168.1.10:8000/bot（见 main.c 的 SERVER_WS_URI）
 *   ctrl_cb ：收到服务器 JSON 控制帧时调用
 * 必须在 comm_connect() / comm_start_playback_task() 之前调用一次。 */
esp_err_t comm_init(const char *uri, comm_ctrl_cb_t ctrl_cb);

/* 注册"连上服务器"回调（可选） */
void comm_register_connected_cb(comm_connected_cb_t cb);

/* 注册"播放开始/结束"回调（用来驱动状态机的 SPEAKING ↔ IDLE） */
void comm_register_playback_cbs(comm_play_start_cb_t start, comm_play_end_cb_t end);

/* 开始连接服务器（异步：连上后触发 connected 回调 + 事件） */
esp_err_t comm_connect(void);

/* 当前是否连着服务器 */
bool comm_is_connected(void);

/* 发送 JSON 控制文本帧，例如 comm_send_json("{\"type\":\"audio_end\"}") */
esp_err_t comm_send_json(const char *json);

/* 发送一段已编码好的音频包：内部自动在前面加 1 字节 codec 标识。
 * 一般不直接调，改用下面的 comm_send_audio_frames()。 */
esp_err_t comm_send_audio(uint8_t codec, const uint8_t *data, size_t len);

/* 把一段 PCM（samples 个 int16 样本）切成 20ms 帧 → Opus 编码 → 逐帧发出。
 * 状态机在 VAD 检测到"说完了"之后调用它把用户语音上传给服务器。 */
esp_err_t comm_send_audio_frames(const int16_t *pcm, size_t samples);

/* 启动内部"接收音频→解码→播放"任务（需在 audio_out / opus_codec 初始化之后调用） */
esp_err_t comm_start_playback_task(void);

/* 通知通信层"服务器这轮音频发完了"（播放任务据此在队列排空后回调 on_play_end） */
void comm_mark_audio_end(void);

/* 极简 JSON 取值：从 json 里找 "key":"value"，把 value 拷到 out（不含引号）。
 * 找到返回 0，没找到返回 -1。够应付 transcript/reply 这类简单字段。 */
int comm_json_get_str(const char *json, const char *key, char *out, size_t out_len);
