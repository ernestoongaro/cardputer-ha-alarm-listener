#!/usr/bin/env python3
"""Measure burst timing, frequency and tonal purity of an alarm recording.

Usage: analyze_alarm_audio.py recording.qta|.m4a|.wav [...]
Converts via ffmpeg to 16 kHz mono (the Cardputer's mic rate) and prints one
line per detected burst so values can be compared with src/alarm_detector.cpp.
"""
import subprocess, sys, tempfile, wave
import numpy as np

WIN_S = 0.02  # 20 ms frames, same as AlarmDetector::kBlockSamples

def load(path):
    tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False).name
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path, "-ac", "1", "-ar", "16000", tmp], check=True)
    w = wave.open(tmp)
    return w.getframerate(), np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float)

def analyze(path):
    sr, x = load(path)
    win = int(sr * WIN_S)
    rows = []
    for i in range(0, len(x) - win, win):
        f = x[i:i + win] * np.hanning(win)
        sp = np.abs(np.fft.rfft(f))
        freqs = np.fft.rfftfreq(win, 1 / sr)
        rows.append((i / sr, np.sqrt((x[i:i + win] ** 2).mean()), freqs[sp.argmax()], sp.max() ** 2 / (sp ** 2).sum()))
    rows = np.array(rows)
    floor = np.percentile(rows[:, 1], 20)
    loud = rows[:, 1] > max(4 * floor, 200)
    bursts, s = [], None
    for k, l in enumerate(loud):
        if l and s is None: s = k
        if not l and s is not None: bursts.append((s, k)); s = None
    if s is not None: bursts.append((s, len(loud)))
    print(f"\n== {path}: {len(x)/sr:.1f}s, noise floor rms≈{floor:.0f}")
    print("   start_s   dur_ms  gap_ms  peak_Hz  tonal_ratio  rms")
    for j, (a, b) in enumerate(bursts):
        seg = rows[a:b]
        gap = (rows[bursts[j + 1][0], 0] - rows[b - 1, 0]) * 1000 if j + 1 < len(bursts) else float("nan")
        print(f"   {seg[0,0]:7.2f}  {(b-a)*int(WIN_S*1000):6d}  {gap:6.0f}  {np.median(seg[:,2]):7.0f}  {np.median(seg[:,3]):11.3f}  {seg[:,1].mean():.0f}")

for p in sys.argv[1:]:
    analyze(p)
