"""BPM, beats, downbeats, time signature and tempo map (docs/07 §1, own implementation).

Onset strength -> autocorrelation tempo with a 120 BPM prior -> dynamic-programming beat
tracker (Ellis 2007) -> sub-frame refinement -> tempo segments by moving median.
Downbeats: bar-phase that best lines up kick accents and harmony changes.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy import ndimage

from .audio import CHROMA_HOP, HOP, ONSET_LAG, SR, Features, frame_time, onset_time, sync

MIN_BPM, MAX_BPM = 55.0, 215.0


def _normalise(o: np.ndarray, fps: float) -> np.ndarray:
    w = max(3, int(round(fps * 1.0)))
    o = o - ndimage.uniform_filter1d(o, w, mode="nearest")
    o = np.maximum(o, 0.0)
    sd = o.std()
    return o / sd if sd > 1e-9 else o


def active_range(rms: np.ndarray, fps: float):
    """First and last frame where the music is audible (-45 dB below the loudest second)."""
    sm = ndimage.uniform_filter1d(rms, max(1, int(fps * 0.2)), mode="nearest")
    if sm.max() <= 0:
        return 0, 0
    thr = sm.max() * 10 ** (-45 / 20)
    on = np.flatnonzero(sm > thr)
    if len(on) == 0:
        return 0, 0
    return int(on[0]), int(on[-1]) + 1


def estimate_period(o: np.ndarray, fps: float):
    """Beat period (frames, fractional) and a 0..1 periodicity strength."""
    n = len(o)
    if n < 8:
        return fps * 0.5, 0.0
    x = ndimage.gaussian_filter1d(o, 1.5)
    x = x - x.mean()
    spec = np.fft.rfft(x, 2 * n)
    ac = np.fft.irfft(spec * np.conj(spec))[:n]
    if ac[0] <= 0:
        return fps * 0.5, 0.0
    ac = ac / ac[0]
    lags = np.arange(n, dtype=np.float64)
    lo = int(np.floor(60.0 * fps / MAX_BPM))
    hi = min(n - 1, int(np.ceil(60.0 * fps / MIN_BPM)))
    if hi <= lo + 2:
        return fps * 0.5, 0.0
    bpm = 60.0 * fps / np.maximum(lags, 1e-9)
    prior = np.exp(-0.5 * (np.log2(bpm / 120.0) / 0.9) ** 2)
    score = np.zeros(n)
    for k in range(lo, hi + 1):
        s = ac[k]
        if 2 * k + 1 < n:
            s += 0.5 * ac[2 * k - 1:2 * k + 2].max()   # a true beat period also repeats at 2x
        score[k] = s * prior[k]
    k = lo + int(np.argmax(score[lo:hi + 1]))
    # Parabolic interpolation of the peak.
    if lo < k < hi:
        a, b, c = score[k - 1], score[k], score[k + 1]
        d = a - 2 * b + c
        frac = 0.5 * (a - c) / d if abs(d) > 1e-12 else 0.0
    else:
        frac = 0.0
    return k + float(np.clip(frac, -0.5, 0.5)), float(np.clip(ac[k], 0.0, 1.0))


def track_beats(o: np.ndarray, period: float, tightness: float = 100.0) -> np.ndarray:
    """Dynamic-programming beat tracker; returns beat frame indices."""
    n = len(o)
    if n == 0:
        return np.zeros(0, dtype=int)
    p = period
    lo, hi = int(np.round(-2 * p)), -int(np.round(p / 2))
    offs = np.arange(lo, hi + 1)
    penalty = -tightness * np.log(-offs / p) ** 2
    score = o.astype(np.float64).copy()
    back = np.full(n, -1, dtype=int)
    for t in range(n):
        idx = t + offs
        ok = idx >= 0
        if not ok.any():
            continue
        cand = score[idx[ok]] + penalty[ok]
        j = int(np.argmax(cand))
        if cand[j] > 0:
            score[t] = o[t] + cand[j]
            back[t] = idx[ok][j]
    # End on the best-scoring frame within the last period.
    tail = max(0, n - int(np.ceil(p)))
    t = tail + int(np.argmax(score[tail:]))
    beats = [t]
    while back[t] >= 0:
        t = back[t]
        beats.append(t)
    return np.array(beats[::-1], dtype=int)


def refine_times(o: np.ndarray, beats: np.ndarray, fps: float) -> np.ndarray:
    """Beat times in seconds, moved to the local onset peak with sub-frame interpolation."""
    out = np.empty(len(beats))
    for i, b in enumerate(beats):
        a, e = max(0, b - 2), min(len(o), b + 3)
        k = a + int(np.argmax(o[a:e]))
        if 0 < k < len(o) - 1 and o[k] > 0:
            y0, y1, y2 = o[k - 1], o[k], o[k + 1]
            d = y0 - 2 * y1 + y2
            frac = 0.5 * (y0 - y2) / d if abs(d) > 1e-12 else 0.0
            k = k + float(np.clip(frac, -0.5, 0.5))
        out[i] = float(k)
    return onset_time(out)


@dataclass
class TempoResult:
    bpm: float
    confidence: float
    beats: np.ndarray                    # seconds
    downbeat_index: int                  # index into beats of the first downbeat
    numerator: int
    meter_confidence: float
    tempo_map: list = field(default_factory=list)   # [{"beat", "bpm", "timeSec"}] beat 0 = first downbeat
    variable: bool = False
    grid_start: float = 0.0              # seconds of bar 1 beat 1 (the tempo map's beat 0)

    @property
    def first_downbeat(self) -> float:
        return float(self.beats[self.downbeat_index]) if len(self.beats) else 0.0


def _robust_fit(idx: np.ndarray, t: np.ndarray):
    """time = intercept + slope * index, ignoring beats more than 3 MADs off the line."""
    if len(idx) < 2:
        return (float(t[0]) if len(t) else 0.0), 0.5
    slope, icpt = np.polyfit(idx, t, 1)
    for _ in range(2):
        res = t - (icpt + slope * idx)
        mad = np.median(np.abs(res - np.median(res))) + 1e-4
        keep = np.abs(res) <= 3.0 * mad + 0.002
        if keep.sum() < 2:
            break
        slope, icpt = np.polyfit(idx[keep], t[keep], 1)
    return float(icpt), float(slope)


def _segments(beats: np.ndarray):
    """Tempo segments as (first beat index, last beat index). A new segment starts where the
    9-beat moving-median tempo leaves the current segment's tempo by more than 2 % for at
    least 8 beats (docs/07 §3)."""
    ibi = np.diff(beats)
    local = 60.0 / ibi
    med = ndimage.median_filter(local, size=9, mode="nearest")
    starts = [0]
    ref = float(np.median(med[:8]))
    i = 0
    while i < len(med):
        if abs(med[i] - ref) / ref > 0.02:
            run = med[i:i + 8]
            if len(run) == 8 and np.all(np.abs(run - ref) / ref > 0.02):
                # The change happens where the raw tempo first crosses halfway.
                new = float(np.median(run))
                j = i
                while j > starts[-1] + 1 and abs(local[j - 1] - new) < abs(local[j - 1] - ref):
                    j -= 1
                starts.append(j)
                ref = new
                i += 8
                continue
        i += 1
    ends = [s for s in starts[1:]] + [len(beats) - 1]
    return list(zip(starts, ends))


def _meter(beats: np.ndarray, feats: Features, hint: int = 0):
    """(numerator, first downbeat index, confidence) from kick accents + harmonic change.
    hint 2..12: the user knows the beats per bar; only the downbeat (phase) is searched, and
    the confidence is how clearly one phase wins."""
    if hint and not 2 <= hint <= 12:
        hint = 0
    if len(beats) < 9:
        return (hint or 4), 0, 0.0
    bounds = np.concatenate([beats, [beats[-1] + (beats[-1] - beats[-2])]])
    # Accent on the beat itself (+-1 frame).
    fr = np.round((beats - ONSET_LAG) * feats.fps).astype(int)
    kick = np.array([feats.onset_low[max(0, k - 1):k + 2].max(initial=0.0) for k in fr])
    ct = frame_time(np.arange(feats.chroma.shape[1]), hop=CHROMA_HOP)
    ch = sync(feats.chroma, ct, bounds)
    ch = ch / (np.linalg.norm(ch, axis=0, keepdims=True) + 1e-9)
    change = np.zeros(len(beats))
    change[1:] = 1.0 - np.sum(ch[:, 1:] * ch[:, :-1], axis=0)

    def z(v):
        s = v.std()
        return (v - v.mean()) / s if s > 1e-9 else v * 0.0

    d = z(kick) + z(change)
    best = None
    scores = {}
    for m in ((hint,) if hint else (3, 4)):
        for ph in range(m):
            on = d[ph::m]
            off = np.delete(d, np.arange(ph, len(d), m))
            s = on.mean() - off.mean()
            if m == 3 and not hint:
                s *= 0.85   # 4/4 is far more common in band repertoire: 3/4 must win clearly
            scores[(m, ph)] = s
            if best is None or s > scores[best]:
                best = (m, ph)
    ordered = sorted(scores.values(), reverse=True)
    if hint:
        other_m = ordered[1] if len(ordered) > 1 else 0.0   # the next best downbeat position
    else:
        other_m = max(v for (m, _), v in scores.items() if m != best[0])
    margin = scores[best] - other_m
    conf = float(np.clip(margin / 1.0, 0.0, 1.0)) if ordered[0] > 0 else 0.0
    return best[0], best[1], conf


def analyse_tempo(feats: Features, progress=None, meter_hint: int = 0) -> TempoResult:
    fps = feats.fps
    o = _normalise(feats.onset.astype(np.float64), fps)
    a, b = active_range(feats.rms, fps)
    if b - a < fps * 4:
        raise ValueError("less than 4 seconds of audible music")
    seg = o[a:b]
    period, strength = estimate_period(seg, fps)
    if progress:
        progress(0.3)
    frames = track_beats(seg, period) + a
    frames = frames[(frames >= a) & (frames < b)]
    beats = refine_times(o, frames, fps)
    if progress:
        progress(0.6)
    if len(beats) < 4:
        raise ValueError("no steady beat found")

    num, phase, meter_conf = _meter(beats, feats, meter_hint)

    segs = _segments(beats)
    fits = []
    for (i0, i1) in segs:
        idx = np.arange(i0, i1 + 1, dtype=np.float64)
        fits.append(_robust_fit(idx, beats[i0:i1 + 1]))
    # Main tempo: the segment with the most beats.
    main = int(np.argmax([i1 - i0 for (i0, i1) in segs]))
    bpm = 60.0 / fits[main][1]

    # Grid origin: the first downbeat on the fitted line of the first segment, moved back
    # bar by bar while it is still inside the music (a pickup before bar 1 stays negative).
    icpt0, slope0 = fits[0]
    start_t = a / fps
    first = phase
    while first - num >= -64 * num and icpt0 + slope0 * (first - num) >= max(0.0, start_t - 0.5 * slope0):
        first -= num
    grid0 = icpt0 + slope0 * first
    tmap = []
    for k, ((i0, i1), (icpt, slope)) in enumerate(zip(segs, fits)):
        gb = 0 if k == 0 else int(i0 - first)
        tmap.append({"beat": gb, "bpm": round(float(60.0 / slope), 3),
                     "timeSec": float(grid0 if k == 0 else icpt + slope * i0)})
    # The MTR rebuilds times from beats and BPMs alone: make each segment's BPM land exactly on
    # the next segment's fitted start, so the grid cannot drift across a tempo change.
    for k in range(len(tmap) - 1):
        nb = tmap[k + 1]["beat"] - tmap[k]["beat"]
        dt = tmap[k + 1]["timeSec"] - tmap[k]["timeSec"]
        if nb > 0 and dt > 0:
            tmap[k]["bpm"] = round(60.0 * nb / dt, 3)
    for k in range(1, len(tmap)):
        prev = tmap[k - 1]
        tmap[k]["timeSec"] = prev["timeSec"] + (tmap[k]["beat"] - prev["beat"]) * 60.0 / prev["bpm"]
    for p in tmap:
        p["timeSec"] = round(p["timeSec"], 4)

    # Confidence: beat regularity inside segments x periodicity strength x beats on onsets.
    ibi = np.diff(beats)
    med = ndimage.median_filter(ibi, size=9, mode="nearest")
    jitter = float(np.median(np.abs(ibi - med) / med))
    regular = float(np.clip(1.0 - jitter / 0.05, 0.0, 1.0))
    on_beat = float(np.clip(np.mean([o[min(len(o) - 1, int(round((t - ONSET_LAG) * fps)))] for t in beats]) / 3.0, 0.0, 1.0))
    conf = float(np.clip(0.5 * regular + 0.25 * strength / 0.5 + 0.25 * on_beat, 0.0, 1.0))
    if progress:
        progress(1.0)
    return TempoResult(bpm=round(bpm, 3), confidence=round(conf, 3), beats=beats, downbeat_index=int(phase),
                       numerator=int(num), meter_confidence=round(meter_conf, 3), tempo_map=tmap,
                       variable=len(tmap) > 1, grid_start=round(float(grid0), 4))


def bar_starts(t: TempoResult) -> np.ndarray:
    """Downbeat times (s) from the first downbeat on."""
    return t.beats[t.downbeat_index::t.numerator]


__all__ = ["analyse_tempo", "TempoResult", "bar_starts", "estimate_period", "track_beats", "HOP", "SR"]
