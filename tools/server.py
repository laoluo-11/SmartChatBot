#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""语音机器人 AI 服务 —— 阿里云百炼 Qwen 大模型 + DashScope TTS。

链路：ESP32 录音 → server 收集 PCM → Qwen 文字回复 → DashScope TTS 合成语音 → ESP32 播放。

运行：
  1) 复制 tools/.env.example 为 tools/.env，填入你的百炼 API Key
  2) pip install -r tools/requirements.txt
  3) python tools/server.py [端口]          # 默认 8000
     python tools/server.py --echo [端口]   # 诊断模式：原样回声（不调 AI）

  也可直接设置环境变量（优先级高于 .env 文件）：
    Windows:  set QWEN_API_KEY=sk-xxx && python tools/server.py
    Linux:    QWEN_API_KEY=sk-xxx python tools/server.py

协议（与固件 comm.c 一致）：
  上行音频：WebSocket 二进制帧 = [1字节 codec] + 负载  (codec=0x01 Opus / 0x02 PCM)
  上行控制：WebSocket 文本帧 JSON，如 {"type":"audio_end"}
  下行音频：二进制帧 [codec=0x02 PCM] + 1920 字节 int16 PCM（960 样本 @16kHz = 60ms）
  下行控制：文本帧 JSON，如 {"type":"transcript","text":"..."} / {"type":"audio_end"}
"""

import asyncio
import base64
import io
import json
import math
import os
import struct
import sys
import time
import wave
from dataclasses import dataclass, field

import numpy as np

try:
    import websockets
except ImportError:
    print("需要先安装依赖：pip install websockets")
    sys.exit(1)

try:
    from openai import OpenAI
except ImportError:
    print("需要先安装依赖：pip install openai>=1.52.0")
    sys.exit(1)

# ---- DashScope TTS（阿里云原生，支持 PCM_16000HZ_MONO_16BIT 直出）----
try:
    import dashscope
    from dashscope.audio.tts_v2 import SpeechSynthesizer, AudioFormat
    HAS_TTS = True
except ImportError:
    HAS_TTS = False
    print("[!] dashscope 未安装，TTS 不可用。安装: pip install dashscope")

# ---- 可选：Opus 解码（无 libopus 时自动退化为 PCM 模式）----
try:
    import opuslib
    HAS_OPUS = True
except Exception:
    HAS_OPUS = False
    print("[!] Opus 解码不可用（opuslib 未安装或缺少 libopus.dll）。")
    print("    若 ESP32 用 PCM 上传(codec=0x02)则不受影响。")

# ══════════════════════════════════════════════════════════════════════
# 配置 —— 全部从环境变量读取，不写在源码里。
# 创建 tools/.env 文件来设置（参考 tools/.env.example），
# 或直接在终端 export / set 环境变量。
# ══════════════════════════════════════════════════════════════════════

def _load_dotenv():
    """自动加载同目录下的 .env 文件（不依赖 python-dotenv 包）。"""
    env_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")
    if not os.path.isfile(env_path):
        return
    with open(env_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            key, val = key.strip(), val.strip().strip("\"'")
            if key and key not in os.environ:  # 环境变量优先级更高
                os.environ[key] = val

_load_dotenv()

API_KEY       = os.environ.get("QWEN_API_KEY", "")
BASE_URL      = os.environ.get("QWEN_BASE_URL",
                                "https://dashscope.aliyuncs.com/compatible-mode/v1")
# 调试：强制用公共 endpoint（工作空间 endpoint 可能不支持 modalities）
BASE_URL_FORCE = os.environ.get("QWEN_BASE_URL_FORCE", "")
if BASE_URL_FORCE:
    BASE_URL = BASE_URL_FORCE
MODEL         = os.environ.get("QWEN_MODEL", "qwen3-omni-flash")
VOICE         = os.environ.get("QWEN_VOICE", "Cherry")
TTS_MODEL     = os.environ.get("TTS_MODEL", "qwen-audio-3.0-tts-flash")
TTS_VOICE     = os.environ.get("TTS_VOICE", "longanhuan_v3.6")
HISTORY_TURNS = int(os.environ.get("QWEN_HISTORY_TURNS", "5"))
WORKSPACE_ID  = os.environ.get("QWEN_WORKSPACE_ID", "ws-vvchkx3qqa728hg2")

# 音频常量（不需配置）
SAMPLE_RATE_IN  = 16000   # ESP32 上传采样率
SAMPLE_RATE_OUT = 24000   # Qwen TTS 输出采样率
FRAME_SAMPLES   = 960     # 每帧样本数（60ms @16kHz）
CODEC_OPUS      = 0x01
CODEC_PCM       = 0x02

# 诊断模式
ECHO_MODE = "--echo" in sys.argv

# ---- 全局 Opus 解码器（惰性初始化）----
_opus_dec = None


def get_opus_decoder():
    global _opus_dec
    if _opus_dec is None and HAS_OPUS:
        try:
            _opus_dec = opuslib.Decoder(SAMPLE_RATE_IN, 1)
        except Exception as e:
            print(f"[!] Opus 解码器初始化失败: {e}")
    return _opus_dec


# ══════════════════════════════════════════════════════════════════════
# 音频工具
# ══════════════════════════════════════════════════════════════════════

def decode_opus_frame(payload: bytes) -> np.ndarray | None:
    """一帧 Opus → int16 numpy (960 samples)"""
    dec = get_opus_decoder()
    if dec is None:
        return None
    try:
        pcm = dec.decode(payload, FRAME_SAMPLES)
        return np.frombuffer(pcm, dtype=np.int16)
    except Exception as e:
        print(f"[!] Opus 解码失败: {e}")
        return None


def decode_all_frames(frames: list[bytes]) -> np.ndarray | None:
    """解码所有音频帧 → int16 numpy array（16kHz mono）"""
    parts = []
    for fr in frames:
        if len(fr) < 2:
            continue
        codec = fr[0]
        payload = fr[1:]
        if codec == CODEC_PCM:
            parts.append(np.frombuffer(payload, dtype=np.int16))
        elif codec == CODEC_OPUS:
            if HAS_OPUS:
                pcm = decode_opus_frame(payload)
                if pcm is not None:
                    parts.append(pcm)
            else:
                # 静默跳过
                pass
    if not parts:
        return None
    return np.concatenate(parts)


def pcm_to_wav_base64(pcm: np.ndarray, sample_rate: int) -> str:
    """int16 PCM → WAV → base64 字符串"""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm.tobytes())
    return base64.b64encode(buf.getvalue()).decode()


def resample_24k_to_16k(audio_24k: np.ndarray) -> np.ndarray:
    """24kHz → 16kHz 线性插值降采样。1440 入 → 960 出 = 正好 1 帧。"""
    n_in = len(audio_24k)
    if n_in < 2:
        return np.array([], dtype=np.int16)
    n_out = max(1, int(n_in * 2 / 3))
    idx = np.linspace(0, n_in - 1, n_out)
    lo = np.floor(idx).astype(int)
    hi = np.minimum(lo + 1, n_in - 1)
    frac = idx - lo
    return (audio_24k[lo].astype(np.float64) * (1.0 - frac) +
            audio_24k[hi].astype(np.float64) * frac).astype(np.int16)


# ══════════════════════════════════════════════════════════════════════
# 流式音频缓冲（处理 Qwen 返回的 base64 PCM chunks）
# ══════════════════════════════════════════════════════════════════════

@dataclass
class StreamAudioBuf:
    """累积 Qwen3-Omni 流式返回的 base64 音频片段，边界安全地解码并降采样。"""
    raw: bytearray = field(default_factory=bytearray)   # 24kHz int16 PCM 原始字节

    def feed(self, b64_str: str):
        """喂入一段 base64 编码的 24kHz PCM。自动过滤空白字符。"""
        clean = b64_str.replace("\n", "").replace("\r", "").replace(" ", "")
        if not clean:
            return
        try:
            decoded = base64.b64decode(clean)
        except Exception:
            # base64 可能在 chunk 边界被截断 — 累积到下一个 chunk 再试
            return
        self.raw.extend(decoded)

    def has_frame(self) -> bool:
        """是否有足够数据出至少一帧（1440 个 24kHz 样本 = 960 个 16kHz 样本）"""
        return len(self.raw) >= 1440 * 2  # int16 = 2 bytes

    def pop_frames(self) -> np.ndarray:
        """取出所有可用的 16kHz PCM，返回 int16 numpy array。"""
        if not self.has_frame():
            return np.array([], dtype=np.int16)
        # 对齐到 int16 边界（2 字节）
        take = (len(self.raw) // 2) * 2
        pcm_24k = np.frombuffer(self.raw[:take], dtype=np.int16)
        self.raw = self.raw[take:]
        return resample_24k_to_16k(pcm_24k)

    def flush(self) -> np.ndarray:
        """清空缓冲区剩余数据（用零补齐对齐后降采样）"""
        if len(self.raw) == 0:
            return np.array([], dtype=np.int16)
        # 补齐到 int16 边界
        if len(self.raw) % 2:
            self.raw.append(0)
        pcm_24k = np.frombuffer(bytes(self.raw), dtype=np.int16)
        self.raw = bytearray()
        if len(pcm_24k) == 0:
            return np.array([], dtype=np.int16)
        return resample_24k_to_16k(pcm_24k)


# ══════════════════════════════════════════════════════════════════════
# 回声模式（诊断用）
# ══════════════════════════════════════════════════════════════════════

REC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recordings")


def save_wav(pcm_bytes: bytes) -> str:
    os.makedirs(REC_DIR, exist_ok=True)
    path = os.path.join(REC_DIR, time.strftime("rec_%Y%m%d_%H%M%S.wav"))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE_IN)
        w.writeframes(pcm_bytes)
    return path


async def echo_handler(ws):
    print("[+] 设备已连接（回声模式）")
    audio_frames = []
    try:
        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                audio_frames.append(bytes(message))
            else:
                try:
                    msg = json.loads(message)
                except json.JSONDecodeError:
                    continue
                if msg.get("type") == "audio_end":
                    pcm_all = b"".join(fr[1:] for fr in audio_frames)
                    if pcm_all:
                        path = save_wav(pcm_all)
                        print(f"[*] 录音已存盘: {path}")
                    await ws.send(json.dumps({"type": "transcript", "text": "你好（回声模式）"}))
                    await ws.send(json.dumps({"type": "reply", "text": "我已收到你的语音"}))
                    for fr in audio_frames:
                        await ws.send(fr)
                        await asyncio.sleep(0.055)
                    await ws.send(json.dumps({"type": "audio_end"}))
                    print(f"[*] 收到一段语音（{len(audio_frames)} 帧），已回传")
                    audio_frames = []
    except websockets.ConnectionClosed:
        pass
    print("[-] 设备断开")


# ══════════════════════════════════════════════════════════════════════
# AI 模式（核心）
# ══════════════════════════════════════════════════════════════════════

def build_system_prompt() -> str:
    return (
        "你是一个友好的中文语音助手，名叫小智。"
        "请用中文简短回答用户的问题。"
        "回答要口语化、自然，就像朋友聊天一样。"
        "尽量控制在 2-5 句话以内，不要啰嗦。"
    )


async def ai_handler(ws):
    print("[+] 设备已连接（AI 模式）")
    client = OpenAI(api_key=API_KEY, base_url=BASE_URL)

    # 对话历史（含 system prompt）
    messages = [{"role": "system", "content": build_system_prompt()}]
    turn_count = 0

    try:
        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                # 音频帧由调用方缓存
                pass
            else:
                try:
                    msg = json.loads(message)
                except json.JSONDecodeError:
                    continue
                if msg.get("type") == "audio_end":
                    # 设备告诉服务器"我这段话说完了"
                    await ws.send(json.dumps({"type": "status", "text": "正在理解..."}))
    except websockets.ConnectionClosed:
        pass
    print("[-] 设备断开")


async def ai_bot_handler(ws):
    """AI 模式：缓存音频 → audio_end → 调 Qwen3-Omni → 流式回传。

    这里不直接用上面的 ai_handler 是因为 async for 循环里
    必须在一层 handler 里同时处理 bytes (collect) 和 text (audio_end)，
    而 ai_handler 里的结构会丢掉音频帧。所以合在一起写。
    """
    print("[+] 设备已连接（AI 模式）")
    client = OpenAI(api_key=API_KEY, base_url=BASE_URL)
    messages = [{"role": "system", "content": build_system_prompt()}]
    audio_frames: list[bytes] = []

    try:
        async for raw in ws:
            if isinstance(raw, (bytes, bytearray)):
                audio_frames.append(bytes(raw))
            else:
                try:
                    obj = json.loads(raw)
                except json.JSONDecodeError:
                    continue

                if obj.get("type") != "audio_end":
                    print("[ctrl]", obj)
                    continue

                # ── audio_end：用户说完了 ──
                if not audio_frames:
                    continue

                await ws.send(json.dumps({"type": "status", "text": "正在理解..."}))

                # 1) 解码所有帧 → PCM → WAV base64
                pcm = decode_all_frames(audio_frames)
                audio_frames = []

                if pcm is None or len(pcm) == 0:
                    await ws.send(json.dumps({"type": "reply", "text": "没有听到声音"}))
                    await ws.send(json.dumps({"type": "audio_end"}))
                    continue

                # 2) 调用 Qwen 生成文字回复（纯文本，不用 modalities）
                messages.append({"role": "user", "content": "请用中文简短、口语化地回答。"})
                transcript = ""

                print("[*] 调用 Qwen...", flush=True)
                try:
                    stream = client.chat.completions.create(
                        model=MODEL,
                        messages=messages,
                        stream=True,
                        stream_options={"include_usage": True},
                    )
                    for chunk in stream:
                        if (chunk.choices
                                and chunk.choices[0].delta
                                and chunk.choices[0].delta.content):
                            text = chunk.choices[0].delta.content
                            transcript += text
                            print(text, end="", flush=True)
                    print()

                except Exception as e:
                    print(f"\n[!] Qwen 调用失败: {e}")
                    await ws.send(json.dumps({"type": "reply", "text": f"抱歉: {e}"}))
                    await ws.send(json.dumps({"type": "audio_end"}))
                    continue

                if not transcript.strip():
                    await ws.send(json.dumps({"type": "reply", "text": "没想好怎么回"}))
                    await ws.send(json.dumps({"type": "audio_end"}))
                    continue

                # 3) DashScope TTS：文字 → 16kHz PCM 语音
                if HAS_TTS:
                    print(f"[*] TTS 合成中（{TTS_MODEL}/{TTS_VOICE}）...", flush=True)
                    try:
                        from dashscope.audio.tts_v2 import SpeechSynthesizer, AudioFormat

                        # 配置 workspace 专用 WebSocket 地址
                        dashscope.api_key = API_KEY
                        dashscope.base_websocket_api_url = (
                            f"wss://{WORKSPACE_ID}.cn-beijing.maas.aliyuncs.com"
                            f"/api-ws/v1/inference"
                        )

                        syn = SpeechSynthesizer(
                            model=TTS_MODEL,
                            voice=TTS_VOICE,
                            format=AudioFormat.PCM_16000HZ_MONO_16BIT,
                        )
                        audio_bytes = await asyncio.to_thread(syn.call, transcript.strip())
                        pcm = np.frombuffer(audio_bytes, dtype=np.int16)
                        print(f"[*] TTS 返回 {len(pcm)} 样本 PCM")
                        await send_pcm_frames(ws, pcm)

                    except Exception as e:
                        print(f"[!] TTS 失败: {e}")
                else:
                    print("[!] TTS 不可用（需 pip install dashscope）")

                # 4) 发送文本 & 标记音频结束
                if transcript:
                    await ws.send(json.dumps({"type": "transcript",
                                               "text": transcript.strip()}))
                    # 保存 assistant 回复到历史
                    assistant_msg = {"role": "assistant",
                                     "content": transcript.strip()}
                    messages.append(assistant_msg)

                await ws.send(json.dumps({"type": "audio_end"}))
                print(f"[*] 回复完成")

                # 5) 裁剪历史防止 token 爆炸
                max_msgs = HISTORY_TURNS * 2 + 1  # system + N轮(user+assistant)
                if len(messages) > max_msgs:
                    messages = [messages[0]] + messages[-(max_msgs - 1):]

    except websockets.ConnectionClosed:
        pass
    print("[-] 设备断开")


async def send_pcm_frames(ws, pcm_16k: np.ndarray) -> int:
    """把 16kHz PCM 切成 960 样本帧，加 codec=0x02 头，逐帧发送。
    间隔 ~55ms（略快于实时 60ms），让 ESP32 播放队列不欠载。
    返回发送的帧数。"""
    sent = 0
    for i in range(0, len(pcm_16k) - FRAME_SAMPLES + 1, FRAME_SAMPLES):
        frame = pcm_16k[i:i + FRAME_SAMPLES]
        await ws.send(bytes([CODEC_PCM]) + frame.tobytes())
        await asyncio.sleep(0.055)
        sent += 1
    return sent


# ══════════════════════════════════════════════════════════════════════
# 入口
# ══════════════════════════════════════════════════════════════════════

async def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = int(args[0]) if args else 8000

    handler = echo_handler if ECHO_MODE else ai_bot_handler

    if not ECHO_MODE:
        # 快速自检
        if not API_KEY or "sk-" not in API_KEY:
            print("[!] 未配置 QWEN_API_KEY。请设置环境变量或创建 tools/.env 文件。")
            print("    参考 tools/.env.example 模板。")
            sys.exit(1)
        if not HAS_OPUS:
            print("[!] 提示：Opus 帧解码需要 opuslib。若不是 Opus 上传则忽略此提示。")

    async with websockets.serve(handler, "0.0.0.0", port,
                                ping_interval=20, max_size=2**24):
        mode = "回声诊断" if ECHO_MODE else f"AI ({MODEL})"
        print(f"语音服务已启动 [{mode}]： ws://0.0.0.0:{port}/bot")
        print(f"ESP32 的 SERVER_WS_URI 应指向本机 IP，如 ws://192.168.x.x:{port}/bot")
        if not ECHO_MODE:
            print(f"音色: {VOICE}  |  对话记忆: {HISTORY_TURNS} 轮")
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
