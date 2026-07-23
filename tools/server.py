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
import sys

try:
    import websockets
except ImportError:
    print("需要先安装依赖：pip install websockets")
    sys.exit(1)

CODEC_OPUS = 0x01
CODEC_PCM = 0x02


async def bot_handler(ws):
    """处理一个设备连接：把收到的音频原样回声，并在 audio_end 时回文本/结束标记。"""
    print("[+] 设备已连接")
    audio_buf = bytearray()
    try:
        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                codec = message[0]
                payload = message[1:]
                audio_buf += payload
                # 回声：把收到的音频原样发回（codec 不变，设备自己解码）
                await ws.send(bytes([codec]) + payload)
            else:
                try:
                    msg = json.loads(message)
                except json.JSONDecodeError:
                    continue
                t = msg.get("type")
                if t == "audio_end":
                    # 回一段"识别/回复"文本（演示用占位）
                    await ws.send(json.dumps({"type": "transcript", "text": "你好"}))
                    await ws.send(json.dumps({"type": "reply", "text": "我已收到你的语音"}))
                    # 回声发完后，告诉设备这轮音频结束
                    await ws.send(json.dumps({"type": "audio_end"}))
                    print(f"[*] 收到一段语音（{len(audio_buf)} 字节），已回声")
                    audio_buf = bytearray()
                else:
                    print("[ctrl]", msg)
    except websockets.ConnectionClosed:
        pass
    print("[-] 设备断开")


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    async with websockets.serve(bot_handler, "0.0.0.0", port, ping_interval=20):
        print(f"WebSocket 回声测试服务已启动： ws://0.0.0.0:{port}/bot")
        print("设备 SERVER_WS_URI 请指向本机 IP，例如 ws://192.168.x.x:8000/bot")
        await asyncio.Future()  # 一直运行


if __name__ == "__main__":
    asyncio.run(main())
