# -*- coding: utf-8 -*-
"""
录音法医分析（纯标准库版）：定位“金属声/汽车人声”的破坏模式
用法: python analyze_wav.py rec_xxx.wav [...]
检查项：
  1) 奇偶样本能量差  —— 奇/偶样本一半≈0或差异巨大 → I2S 槽位/声道问题（隔样本污染）
  2) lag-1 自相关    —— 正常语音 >0.9；接近 0/负 → 相邻样本互不相关（错位/交叠破坏）
  3) 字节交换检验    —— 高低字节互换后自相关反而变好 → 字节序错位
  4) 移1字节检验     —— 整体错开1字节后自相关变好 → 流错了奇数字节
  5) 周期性坏点      —— 每 N 样本边界跳变异常 → 块边界破坏（DMA/分片/缓冲拼接）
"""
import sys, wave, struct, math

def load(path):
    w = wave.open(path, "rb")
    n = w.getnframes()
    raw = w.readframes(n)
    sr, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
    w.close()
    cnt = len(raw) // 2
    a = list(struct.unpack("<%dh" % cnt, raw[:cnt*2]))
    return a, raw, sr, ch, sw

def rms(x):
    if not x: return 0.0
    return math.sqrt(sum(float(v)*v for v in x) / len(x))

def lag1(x):
    n = len(x)
    if n < 3: return 0.0
    m = sum(x) / n
    num = den1 = den2 = 0.0
    for i in range(n - 1):
        a = x[i] - m; b = x[i+1] - m
        num += a * b; den1 += a * a; den2 += b * b
    if den1 == 0 or den2 == 0: return 0.0
    return num / math.sqrt(den1 * den2)

def analyze(path):
    a, raw, sr, ch, sw = load(path)
    print(f"==== {path} ====")
    print(f"采样率={sr}Hz 声道={ch} 位宽={sw*8}bit 样本数={len(a)} 时长={len(a)/sr:.2f}s")
    if len(a) < 2000:
        print("太短，跳过\n"); return

    # 找能量最大的 0.5 秒段（大概率是语音段）
    win = sr // 2
    step = win // 4
    best_s, best_e = 0, min(win, len(a))
    if len(a) > win:
        best_energy = -1.0
        for s in range(0, len(a) - win, step):
            seg = a[s:s+win]
            e = sum(float(v)*v for v in seg[::4])  # 抽样算能量，加速
            if e > best_energy:
                best_energy = e; best_s = s
        best_e = best_s + win
    seg = a[best_s:best_e]
    print(f"[选段] 样本 {best_s}~{best_e}（能量最大的 0.5s）")

    r_all = rms(seg)
    peak = max(abs(v) for v in seg)
    dc = sum(seg) / len(seg)
    clip = sum(1 for v in seg if abs(v) > 32000) * 100.0 / len(seg)
    print(f"[健康度] RMS={r_all:.0f} 峰值={peak} 直流={dc:.1f} 削波占比={clip:.2f}%")

    # 1) 奇偶样本
    ev, od = seg[0::2], seg[1::2]
    re, ro = rms(ev), rms(od)
    ze = sum(1 for v in ev if v == 0)*100.0/len(ev)
    zo = sum(1 for v in od if v == 0)*100.0/len(od)
    ratio = max(re, ro) / max(min(re, ro), 1e-9)
    print(f"[奇偶] 偶RMS={re:.0f}(零{ze:.1f}%)  奇RMS={ro:.0f}(零{zo:.1f}%)  比值={ratio:.2f}"
          + ("   <-- 隔样本污染!" if ratio > 3 or ze > 40 or zo > 40 else ""))

    # 2) 自相关
    c_all, c_ev, c_od = lag1(seg), lag1(ev), lag1(od)
    print(f"[自相关] 全序列lag1={c_all:.3f}  偶序列={c_ev:.3f}  奇序列={c_od:.3f}")
    print( "          判读: 全<0.5 且 奇/偶各自>0.8 → 隔样本污染(槽位/声道)")

    # 3) 字节交换
    sw_seg = []
    for v in seg:
        u = v & 0xFFFF
        u2 = ((u & 0xFF) << 8) | (u >> 8)
        sw_seg.append(u2 - 65536 if u2 >= 32768 else u2)
    c_sw = lag1(sw_seg)
    print(f"[字节序] 高低字节互换后 lag1={c_sw:.3f}" + ("   <-- 字节序错位!" if c_sw > c_all + 0.3 else ""))

    # 4) 移 1 字节
    off = best_s * 2 + 1
    m = min(len(seg), (len(raw) - off) // 2)
    s1 = list(struct.unpack_from("<%dh" % m, raw, off))
    c_s1 = lag1(s1)
    print(f"[移1字节] 错开1字节重组后 lag1={c_s1:.3f}" + ("   <-- 奇数字节错位!" if c_s1 > c_all + 0.3 else ""))

    # 5) 周期性坏点
    d = [abs(seg[i+1] - seg[i]) for i in range(len(seg) - 1)]
    dm = sum(d) / len(d) + 1e-9
    print("[周期坏点] 块长N: 边界跳变/普通跳变 (>3 可疑)")
    for N in (16, 32, 64, 128, 160, 256, 320, 480, 512, 960, 1024):
        vals = [d[i] for i in range(N - 1, len(d), N)]
        if len(vals) < 8: continue
        r = (sum(vals) / len(vals)) / dm
        print(f"    N={N:5d}: {r:.2f}" + ("   <-- 可疑!" if r > 3 else ""))

    # 6) 简易频谱：Goertzel 检查 sr/2 附近能量（隔样本调制会把能量镜像到高端）
    def band_energy(x, f0, f1, sr, K=24):
        tot = 0.0
        for k in range(K):
            f = f0 + (f1 - f0) * k / (K - 1)
            w = 2 * math.pi * f / sr
            cw, sw_ = math.cos(w), math.sin(w)
            re_ = im_ = 0.0
            # 抽前 4096 点做 DFT 单点（够判断）
            n = min(4096, len(x))
            for i in range(n):
                re_ += x[i] * math.cos(w * i)
                im_ -= x[i] * math.sin(w * i)
            tot += math.sqrt(re_*re_ + im_*im_)
        return tot / K
    e_low  = band_energy(seg, 300, 2000, sr)
    e_high = band_energy(seg, sr//2 - 1500, sr//2 - 100, sr)
    print(f"[频谱] 语音带(300-2000Hz)均值={e_low:.0f}  近奈奎斯特带({sr//2-1500}-{sr//2-100}Hz)均值={e_high:.0f}  高/低={e_high/max(e_low,1e-9):.2f}"
          + ("   <-- 高端异常强(隔样本调制特征)!" if e_high > e_low * 0.5 else ""))
    print()

if __name__ == "__main__":
    for p in sys.argv[1:]:
        analyze(p)
