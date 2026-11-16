#!/usr/bin/env python3
"""Writes a small Standard MIDI File for tests and CI: a chord pattern, pedal and a program change.

usage: make-test-mid.py out.mid [--bars N] [--tpq 480] [--channel 1-16]
"""
import argparse
import struct


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--bars", type=int, default=8)
    ap.add_argument("--tpq", type=int, default=480)
    ap.add_argument("--channel", type=int, default=1)
    a = ap.parse_args()
    ch = (a.channel - 1) & 0x0F
    events = []  # (tick, bytes)
    events.append((0, bytes([0xFF, 0x51, 0x03]) + (500000).to_bytes(3, "big")))  # 120 bpm
    events.append((0, bytes([0xC0 | ch, 48])))
    chords = [(60, 64, 67), (57, 60, 64), (53, 57, 60), (55, 59, 62)]
    for bar in range(a.bars):
        start = (bar + 1) * 4 * a.tpq  # bar 2 onwards
        notes = chords[bar % len(chords)]
        events.append((start, bytes([0xB0 | ch, 64, 127])))
        for n in notes:
            events.append((start, bytes([0x90 | ch, n, 96])))
        for n in notes:
            events.append((start + 4 * a.tpq - a.tpq // 8, bytes([0x80 | ch, n, 0])))
        events.append((start + 4 * a.tpq - a.tpq // 16, bytes([0xB0 | ch, 64, 0])))
    events.sort(key=lambda e: e[0])
    trk = b""
    last = 0
    for t, b in events:
        trk += vlq(t - last) + b
        last = t
    trk += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    with open(a.out, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, a.tpq))
        f.write(b"MTrk" + struct.pack(">I", len(trk)) + trk)


if __name__ == "__main__":
    main()
