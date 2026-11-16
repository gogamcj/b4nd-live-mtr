#!/usr/bin/env python3
"""Writes a stereo 16-bit test WAV (220 Hz left at -6 dBFS, 330 Hz right at -12 dBFS).

Used by CI and for checking outputs on real hardware:
    python tools/make-test-wav.py backing.wav --rate 44100 --seconds 20
Standard library only.
"""
import argparse
import math
import struct
import wave

p = argparse.ArgumentParser()
p.add_argument("out")
p.add_argument("--rate", type=int, default=44100)
p.add_argument("--seconds", type=float, default=20.0)
a = p.parse_args()

n = int(a.rate * a.seconds)
with wave.open(a.out, "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(a.rate)
    frames = bytearray()
    for i in range(n):
        l = int(0.5 * 32767 * math.sin(2 * math.pi * 220 * i / a.rate))
        r = int(0.25 * 32767 * math.sin(2 * math.pi * 330 * i / a.rate))
        frames += struct.pack("<hh", l, r)
    w.writeframes(bytes(frames))
print(f"wrote {a.out}: {a.rate} Hz, {a.seconds} s, stereo")
