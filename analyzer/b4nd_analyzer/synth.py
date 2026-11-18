"""Synthetic test songs with a known answer (tempo, meter, key, chords, sections).

Used by the unit tests and by the app's end-to-end check in CI; never by a real analysis.
"""

from __future__ import annotations

import json
import wave
from dataclasses import dataclass, field

import numpy as np

NOTE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def parse_chord(name: str):
    root = NOTE[name[0]]
    rest = name[1:]
    if rest[:1] == "#":
        root += 1
        rest = rest[1:]
    elif rest[:1] == "b":
        root -= 1
        rest = rest[1:]
    minor = rest.startswith("m")
    return root % 12, minor


@dataclass
class Section:
    name: str             # intro | verse | pre_chorus | chorus | bridge | solo | outro
    chords: list          # one chord per bar, repeated to fill `bars`
    bars: int
    energy: float = 1.0   # 0.3 quiet .. 1.3 loud (also adds brightness)
    bpm: float = 0.0      # 0 = song tempo
    drums: bool = True


@dataclass
class Song:
    bpm: float
    numerator: int = 4
    lead_in: float = 0.5          # seconds of silence before the first downbeat
    sections: list = field(default_factory=list)
    sr: int = 22050
    tail: float = 1.0

    def timeline(self):
        """Per-bar start time, bpm, chord and section index; plus the beat times."""
        t = self.lead_in
        bars, beats = [], []
        for si, s in enumerate(self.sections):
            bpm = s.bpm or self.bpm
            for b in range(s.bars):
                bars.append((t, bpm, s.chords[b % len(s.chords)], si))
                for k in range(self.numerator):
                    beats.append(t + k * 60.0 / bpm)
                t += self.numerator * 60.0 / bpm
        return bars, beats, t

    def truth(self):
        bars, beats, end = self.timeline()
        sections, chords = [], []
        idx = 0
        for si, s in enumerate(self.sections):
            sections.append({"label": s.name, "startSec": bars[idx][0]})
            idx += s.bars
        for i, (t, _, c, _) in enumerate(bars):
            e = bars[i + 1][0] if i + 1 < len(bars) else end
            if chords and chords[-1]["chord"] == c:
                chords[-1]["endSec"] = e
            else:
                chords.append({"startSec": t, "endSec": e, "chord": c})
        return {"bpm": self.bpm, "numerator": self.numerator, "firstDownbeat": self.lead_in,
                "beats": beats, "sections": sections, "chords": chords, "endSec": end}


def _env(n, sr, attack, decay):
    t = np.arange(n) / sr
    a = np.minimum(1.0, t / max(attack, 1e-4))
    return a * np.exp(-t / decay)


def render(song: Song, seed: int = 1) -> np.ndarray:
    sr = song.sr
    rng = np.random.default_rng(seed)
    bars, beats, end = song.timeline()
    n = int((end + song.tail) * sr)
    y = np.zeros(n, dtype=np.float64)

    def add(at, sig):
        i = int(round(at * sr))
        if i >= n:
            return
        m = min(len(sig), n - i)
        y[i:i + m] += sig[:m]

    kick_n = int(0.18 * sr)
    tk = np.arange(kick_n) / sr
    kick = np.sin(2 * np.pi * (50 * tk + 60 * (1 - np.exp(-tk * 30)) / 30)) * _env(kick_n, sr, 0.001, 0.07)
    snare_n = int(0.15 * sr)
    snare = rng.standard_normal(snare_n) * _env(snare_n, sr, 0.001, 0.04) * 0.5
    snare += np.sin(2 * np.pi * 190 * np.arange(snare_n) / sr) * _env(snare_n, sr, 0.001, 0.03) * 0.4
    hat_n = int(0.04 * sr)
    hat = np.diff(rng.standard_normal(hat_n + 1)) * _env(hat_n, sr, 0.0005, 0.01) * 0.12

    for (t0, bpm, chord, si) in bars:
        sec = song.sections[si]
        beat = 60.0 / bpm
        e = sec.energy
        root, minor = parse_chord(chord)
        third = 3 if minor else 4
        # Pad: triad, sustained over the bar; louder sections are brighter.
        bar_len = song.numerator * beat
        pn = int(bar_len * sr)
        tp = np.arange(pn) / sr
        penv = np.minimum(1.0, tp / 0.03) * np.minimum(1.0, (bar_len - tp) / 0.03)
        harmonics = 4 if e < 1.0 else 8
        pad = np.zeros(pn)
        for iv in (0, third, 7):
            f = 261.63 * 2 ** ((root + iv) / 12.0)
            if f > 494:
                f /= 2
            for h in range(1, harmonics + 1):
                pad += np.sin(2 * np.pi * f * h * tp + h) / h
        add(t0, pad * penv * 0.05 * e)
        # Bass: root, one note per beat.
        fb = 65.41 * 2 ** (root / 12.0)
        bn = int(beat * 0.9 * sr)
        tb = np.arange(bn) / sr
        bass = (np.sin(2 * np.pi * fb * tb) + 0.3 * np.sin(4 * np.pi * fb * tb)) * _env(bn, sr, 0.005, 0.25)
        for k in range(song.numerator):
            add(t0 + k * beat, bass * 0.25 * e)
        if not sec.drums:
            continue
        for k in range(song.numerator):
            tb0 = t0 + k * beat
            if song.numerator == 3:
                if k == 0:
                    add(tb0, kick * 0.9 * e)
                else:
                    add(tb0, snare * 0.5 * e)
            else:
                if k in (0, 2):
                    add(tb0, kick * (0.9 if k == 0 else 0.7) * e)
                if k in (1, 3):
                    add(tb0, snare * 0.8 * e)
            add(tb0, hat * e)
            add(tb0 + beat / 2, hat * 0.6 * e)
    peak = np.max(np.abs(y))
    if peak > 0:
        y *= 0.7 / peak
    return y.astype(np.float32)


def write_wav(path: str, y: np.ndarray, sr: int, channels: int = 1):
    pcm = np.clip(np.round(y * 32767.0), -32768, 32767).astype("<i2")
    if channels == 2:
        pcm = np.repeat(pcm[:, None], 2, axis=1)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


SHARP = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def transpose(chord: str, semitones: int) -> str:
    root, minor = parse_chord(chord)
    return SHARP[(root + semitones) % 12] + ("m" if minor else "")


def demo_song(bpm: float = 128.0, numerator: int = 4, lead_in: float = 0.6, sr: int = 22050,
              semitones: int = 0) -> Song:
    """Intro 4 / Verse 8 / Chorus 8 / Verse 8 / Chorus 8 / Outro 4 bars in A minor
    (transposed by `semitones`)."""
    def tr(chords):
        return [transpose(c, semitones) for c in chords]

    verse = tr(["Am", "F", "C", "G"])
    chorus = tr(["F", "G", "Em", "Am"])
    return Song(bpm=bpm, numerator=numerator, lead_in=lead_in, sr=sr, sections=[
        Section("intro", tr(["Am", "Am", "F", "G"]), 4, energy=0.5, drums=False),
        Section("verse", verse, 8, energy=0.8),
        Section("chorus", chorus, 8, energy=1.3),
        Section("verse", verse, 8, energy=0.8),
        Section("chorus", chorus, 8, energy=1.3),
        Section("outro", tr(["Am"]), 4, energy=0.5, drums=False),
    ])


def main_synth(args) -> int:
    song = demo_song(args.bpm, args.numerator, args.lead_in, args.rate)
    if args.tempo_change:
        song.sections[3].bpm = args.tempo_change
        song.sections[4].bpm = args.tempo_change
    y = render(song)
    write_wav(args.out, y, song.sr, args.channels)
    if args.truth:
        with open(args.truth, "w", encoding="utf-8") as f:
            json.dump(song.truth(), f, indent=1)
    return 0
