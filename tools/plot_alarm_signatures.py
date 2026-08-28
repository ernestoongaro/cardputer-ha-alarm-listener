#!/usr/bin/env python3
"""Render the smoke / CO signature figures for the blog from real recordings.

Usage: plot_alarm_signatures.py smoke.wav co.wav out_dir
(16 kHz mono WAVs, e.g. produced by ffmpeg -ac 1 -ar 16000)
"""
import sys, wave
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BG, INK, MUTED, AMBER, AMBER_DIM, CYAN, RED, BAND = ("#0b0f1a", "#f2f4f8", "#8b93a7", "#f5a623", "#5a4a1e", "#3fd0d4", "#ff3b3b", "#0f2a33")
WIN = 0.02

def load(path):
    w = wave.open(path); sr = w.getframerate()
    return sr, np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float)

def frames(x, sr):
    n = int(sr * WIN); out = []
    for i in range(0, len(x) - n, n):
        f = x[i:i + n] * np.hanning(n); sp = np.abs(np.fft.rfft(f))
        out.append((i / sr, np.sqrt((x[i:i + n] ** 2).mean()), np.fft.rfftfreq(n, 1 / sr)[sp.argmax()]))
    return np.array(out)

def figure(path, t0, span, title, subtitle, badge, band, annotate, out):
    sr, x = load(path)
    seg = np.zeros(int(span * sr)); src = x[int(t0 * sr):int((t0 + span) * sr)]; seg[:len(src)] = src
    fr = frames(seg, sr); floor = np.percentile(frames(x, sr)[:, 1], 20)  # quiet part of the whole file
    gate = max(4 * floor, 200)
    # 1 ms min/max envelope of the real waveform
    k = sr // 1000; m = len(seg) // k; blk = seg[:m * k].reshape(m, k)
    tt = np.arange(m) / 1000; hi, lo = blk.max(1), blk.min(1); amp = np.abs(seg).max()

    fig = plt.figure(figsize=(20.8, 10.4), dpi=100, facecolor=BG)
    ax = fig.add_axes([0.085, 0.36, 0.885, 0.46], facecolor=BG)
    fx = fig.add_axes([0.085, 0.17, 0.885, 0.13], facecolor=BG, sharex=ax)
    for a in (ax, fx):
        for s in a.spines.values(): s.set_visible(False)
        a.tick_params(colors=MUTED, labelsize=15, length=0)
    ax.fill_between(tt, lo, hi, color=AMBER_DIM, lw=0)
    ax.plot(tt, hi, color=AMBER, lw=0.8); ax.plot(tt, lo, color=AMBER, lw=0.8)
    ax.axhline(gate, color=CYAN, ls=(0, (6, 5)), lw=1.4); ax.axhline(-gate, color=CYAN, ls=(0, (6, 5)), lw=1.4)
    ax.set_ylim(-amp * 1.35, amp * 1.35); ax.set_xlim(0, span); ax.set_yticks([]); ax.tick_params(labelbottom=False)
    ax.set_ylabel("Level", color=MUTED, fontsize=16)
    ax.text(span, -amp * 1.28, "level gate", color=CYAN, fontsize=16, ha="right")
    fx.axhspan(band[0], band[1], color=BAND, lw=1, ec=CYAN, alpha=0.9)
    loud = fr[fr[:, 1] > gate]
    fx.scatter(loud[:, 0] + WIN / 2, loud[:, 2], s=14, color=AMBER, lw=0)
    fx.set_ylim(band[0] - 350, band[1] + 350); fx.set_yticks(band); fx.set_yticklabels([f"{b/1000:.2f} kHz" for b in band], color=CYAN)
    fx.set_ylabel("Peak\nfrequency", color=MUTED, fontsize=16); fx.set_xlabel("Time (s)", color=MUTED, fontsize=17)
    fx.set_xticks(range(int(span) + 1))
    fig.text(0.085, 0.93, title, color=INK, fontsize=32); fig.text(0.085, 0.88, subtitle, color=MUTED, fontsize=18)
    fig.text(0.97, 0.93, badge, color=RED, fontsize=32, ha="right")
    fig.text(0.5, 0.045, f"Real microphone recording of the installed alarm ({path.split('/')[-1]}), 16 kHz mono; 20 ms analysis frames, the same block size the Cardputer uses",
             color=MUTED, fontsize=15, ha="center")
    annotate(ax, fx, fr, gate, amp, span)
    fig.savefig(out, facecolor=BG); print("wrote", out)

def bursts(fr, gate):
    loud = fr[:, 1] > gate; out, s = [], None
    for i, l in enumerate(loud):
        if l and s is None: s = i
        if not l and s is not None:
            if i - s >= 5: out.append((fr[s, 0], fr[i, 0]))
            s = None
    if s is not None: out.append((fr[s, 0], fr[-1, 0] + WIN))
    return out

def merge(bs, gap=0.15):
    m = [list(bs[0])]
    for a, b in bs[1:]:
        if a - m[-1][1] < gap: m[-1][1] = b
        else: m.append([a, b])
    return m

def vline(ax, fx, t, label):
    for a in (ax, fx): a.axvline(t, color=RED, lw=2.2)
    ax.text(t + 0.04, ax.get_ylim()[1] * 0.93, label, color=RED, fontsize=19)

def ann_smoke(ax, fx, fr, gate, amp, span):
    b = merge(bursts(fr, gate)); t_on = b[0][0]; t_ok = t_on + 3.0; y = amp * 1.12
    ax.annotate("", (t_ok, y), (t_on, y), arrowprops=dict(arrowstyle="<->", color=INK, lw=1.4))
    ax.text((t_on + t_ok) / 2, y + amp * 0.08, "150 consecutive 20 ms frames  =  3.00 s", color=INK, fontsize=19, ha="center")
    vline(ax, fx, t_ok, "confirmed")
    loud = fr[fr[:, 1] > gate]; fx.text(0.02, fx.get_ylim()[1] - 60, f"measured ≈ {np.percentile(loud[:,2],5)/1000:.2f}–{np.percentile(loud[:,2],95)/1000:.2f} kHz, {b[0][1]-b[0][0]:.1f} s unbroken (recording cut at 5 s)", color=MUTED, fontsize=16, ha="left", va="top")

def ann_co(ax, fx, fr, gate, amp, span):
    b = merge(bursts(fr, gate))[:3]; y = amp * 1.12
    ax.annotate("", (b[2][1], y), (b[0][0], y), arrowprops=dict(arrowstyle="<->", color=INK, lw=1.4))
    ax.text((b[0][0] + b[2][1]) / 2, y + amp * 0.08, "3 bursts counted", color=INK, fontsize=19, ha="center")
    for i, (a, e) in enumerate(b):
        seg = fr[(fr[:, 0] >= a) & (fr[:, 0] < e)]
        ax.text((a + e) / 2, -amp * 1.05, f"burst {i+1}\n{e-a:.2f} s · rms {seg[:,1].mean():.0f}", color=INK, fontsize=16, ha="center", va="top")
        if i < 2:
            g0, g1 = e, b[i + 1][0]; yg = amp * 0.75
            ax.annotate("", (g1, yg), (g0, yg), arrowprops=dict(arrowstyle="<->", color=CYAN, lw=1.4))
            ax.text((g0 + g1) / 2, yg + amp * 0.06, f"gap {g1-g0:.2f} s", color=CYAN, fontsize=16, ha="center")
    t_ok = b[2][1] + 0.9; yg = amp * 0.75
    ax.annotate("", (t_ok, yg), (b[2][1], yg), arrowprops=dict(arrowstyle="<->", color=RED, lw=1.4))
    ax.text((b[2][1] + t_ok) / 2, yg + amp * 0.06, "final pause ≥ 0.90 s", color=RED, fontsize=16, ha="center")
    vline(ax, fx, t_ok, "confirmed")
    loud = fr[fr[:, 1] > gate]; fx.text(span, fx.get_ylim()[1] - 60, f"measured ≈ {np.median(loud[:,2])/1000:.2f} kHz in every burst", color=MUTED, fontsize=16, ha="right", va="top")

smoke, co, out = sys.argv[1:4]
figure(smoke, 3.0, 5.0, "Smoke signature — one sustained tone", "A loud 2.80–3.30 kHz tone that simply refuses to stop", "SMOKE  ALARM", (2800, 3300), ann_smoke, f"{out}/smoke-signature.png")
figure(co, 4.0, 5.0, "Carbon monoxide signature — three bursts, then a pause", "Three short 2.85–3.45 kHz bursts, valid gaps, and a deliberate silence", "CO  ALARM", (2850, 3450), ann_co, f"{out}/co-signature.png")
