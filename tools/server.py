#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""语音机器人 L7 参考/测试服务器（回声服务）。

作用：让你在没有"真实 ASR+LLM+TTS 服务器"时，也能把 L7 的整条链路跑通：
  设备采集麦克风 → Opus/PCM 上传 → 服务器原样回声 → 设备解码播放。

运行：
  1) pip install websockets
  2) 把本机 IP 填进 main.c 的 SERVER_WS_URI（如 ws://192.168.1.50:8000/bot）
     注意：设备必须和本机在同一 WiFi（能互相 ping 通）
  3) python tools/server.py 8000        # 端口可省，默认 8000
  4) 设备连上后，按唤醒键说话，停顿时服务器会把音频回传，喇叭应原样播放出来

协议（与固件一致）：
  上行音频：WebSocket 二进制帧 = [1字节 codec] + 音频负载
            codec=0x01 Opus / 0x02 PCM
  上行控制：WebSocket 文本帧 JSON，如 {"type":"audio_end"}
  下行音频：二进制帧 [codec] + 负载（回声原样返回，Opus/PCM 都无需解码）
  下行控制：文本帧 JSON，如 {"type":"transcript","text":"..."} / {"type":"audio_end"}

正式服务器（ASR+LLM+TTS）见配套架构文档 voice-chatbot-architecture.md 第 4 节。
"""

import asyncio
import json
import math
import os
import struct
import sys
import time
import wave

try:
    import websockets
except ImportError:
    print("需要先安装依赖：pip install websockets")
    sys.exit(1)

CODEC_OPUS = 0x01
CODEC_PCM = 0x02

SAMPLE_RATE = 16000          # 与固件一致
FRAME_SAMPLES = 960          # 一帧 960 样本 = 60ms
FRAME_BYTES = FRAME_SAMPLES * 2

# --tone 模式：不回声，改发服务器生成的标准正弦音（用于隔离"播放端"问题）
TONE_MODE = "--tone" in sys.argv

REC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recordings")


def save_wav(pcm_bytes: bytes) -> str:
    """把设备上传的 PCM 存成 WAV 文件（16kHz/单声道/16bit），返回文件路径。
    【诊断用】在电脑上直接听这个文件 = 听麦克风采到的原始数据：
      - WAV 也是"汽车人声" → 问题在麦克风采集/上传半段；
      - WAV 干净          → 问题在下发/播放半段。"""
    os.makedirs(REC_DIR, exist_ok=True)
    path = os.path.join(REC_DIR, time.strftime("rec_%Y%m%d_%H%M%S.wav"))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm_bytes)
    return path


def make_tone_frames(freq=440.0, seconds=2.0, amplitude=8000):
    """生成一段标准正弦音，切成与固件协议一致的帧：[1字节codec] + 1920字节PCM。
    这段数据是数学生成的"完美音频"——如果喇叭播出来仍是金属声，
    则可确证问题在设备的接收/播放路径，而与麦克风无关。"""
    total = int(SAMPLE_RATE * seconds)
    pcm = struct.pack("<%dh" % total,
                      *[int(amplitude * math.sin(2 * math.pi * freq * i / SAMPLE_RATE))
                        for i in range(total)])
    frames = []
    for off in range(0, len(pcm) - FRAME_BYTES + 1, FRAME_BYTES):
        frames.append(bytes([CODEC_PCM]) + pcm[off:off + FRAME_BYTES])
    return frames


async def bot_handler(ws):
    """处理一个设备连接：先缓存整段音频，收到 audio_end 后再原样回声。

    关键改动（v2）：不再"边收边发"。原来每收到一帧就立刻回传，会让设备
    还在上传时就收到回声、把状态误切成 SPEAKING；现在改为缓存到 audio_end
    才整段回传，行为更接近真实 ASR+LLM+TTS 后端（对方收完整段才回话）。
    回传时逐帧加 ~20ms 间隔，避免一次性刷爆设备播放队列。
    """
    print("[+] 设备已连接")
    audio_frames = []   # 缓存完整二进制帧（含 codec 字节），等 audio_end 再回传
    try:
        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                # 只缓存，不立即回声
                audio_frames.append(bytes(message))
            else:
                try:
                    msg = json.loads(message)
                except json.JSONDecodeError:
                    continue
                t = msg.get("type")
                if t == "audio_end":
                    # 【诊断】把设备上传的原始录音存成 WAV（剥掉每帧第 1 字节 codec）
                    pcm_all = b"".join(fr[1:] for fr in audio_frames)
                    if pcm_all:
                        path = save_wav(pcm_all)
                        print(f"[*] 录音已存盘: {path}（电脑上听它=听麦克风原始数据）")

                    # 先回一段"识别/回复"文本（演示用占位）
                    await ws.send(json.dumps({"type": "transcript", "text": "你好"}))
                    await ws.send(json.dumps({"type": "reply", "text": "我已收到你的语音"}))

                    if TONE_MODE:
                        # --tone 模式：不回声，改发服务器生成的"数学完美"440Hz 正弦音。
                        # 喇叭若播出金属声 → 问题在设备接收/播放路径；干净 → 播放路径没问题。
                        send_frames = make_tone_frames()
                        print(f"[*] --tone 模式：回传 440Hz 标准正弦音（{len(send_frames)} 帧）")
                    else:
                        send_frames = audio_frames

                    # 把音频按帧回传。
                    # 【关键】每帧是 960 样本 @16kHz = 60ms 音频（不是 20ms！），
                    # 所以逐帧间隔必须 ~55ms（略快于实时，给网络抖动留一点余量）。
                    # 之前 sleep(0.02) 是 3 倍速灌入 → 设备播放队列(48帧)塞满后
                    # "丢最旧帧"，每丢 1 帧就缺 60ms 声音 → 回声被切碎成"机器人声"。
                    for fr in send_frames:
                        await ws.send(fr)
                        await asyncio.sleep(0.055)
                    # 发完后，告诉设备这轮音频结束
                    await ws.send(json.dumps({"type": "audio_end"}))
                    print(f"[*] 收到一段语音（{len(audio_frames)} 帧），已回传")
                    audio_frames = []
                else:
                    print("[ctrl]", msg)
    except websockets.ConnectionClosed:
        pass
    print("[-] 设备断开")


async def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = int(args[0]) if args else 8000
    async with websockets.serve(bot_handler, "0.0.0.0", port, ping_interval=20):
        print(f"WebSocket 回声测试服务已启动： ws://0.0.0.0:{port}/bot")
        print("设备 SERVER_WS_URI 请指向本机 IP，例如 ws://192.168.x.x:8000/bot")
        if TONE_MODE:
            print("[--tone] 诊断模式：说话后回传 440Hz 标准正弦音（不回声）")
        print(f"[诊断] 每段上传的录音会自动存 WAV 到: {REC_DIR}")
        await asyncio.Future()  # 一直运行


if __name__ == "__main__":
    asyncio.run(main())
