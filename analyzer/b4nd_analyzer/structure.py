"""Song sections (Intro / Verse / Chorus ...) as cue drafts (docs/07 §1, own implementation).

Bar-level chroma + timbre features -> self-similarity -> checkerboard novelty -> boundaries
on bar lines -> segments grouped by similarity -> names from repetition, loudness and place.
"""

from __future__ import annotations

import numpy as np
from scipy import ndimage

from .audio import CHROMA_HOP, Features, frame_time, sync


def _bar_features(feats: Features, bars: np.ndarray):
    ct = frame_time(np.arange(feats.chroma.shape[1]), hop=CHROMA_HOP)
    ft = frame_time(np.arange(feats.bands.shape[1]))
    ch = sync(feats.chroma, ct, bars)
    ch = ch / (np.linalg.norm(ch, axis=0, keepdims=True) + 1e-9)
    tb = sync(feats.bands, ft, bars)
    tb = tb - tb.mean(axis=0, keepdims=True)           # spectral shape, not loudness
    tb = tb / (np.linalg.norm(tb, axis=0, keepdims=True) + 1e-9)
    rms = sync(feats.rms[None, :], ft, bars)[0]
    loud = 20 * np.log10(rms / (rms.max() + 1e-12) + 1e-6)
    lz = (loud - loud.mean()) / (loud.std() + 1e-9)
    x = np.vstack([ch, 0.7 * tb, 0.25 * lz[None, :]])
    x = x - x.mean(axis=1, keepdims=True)     # contrast: what differs between bars
    x = x / (np.linalg.norm(x, axis=0, keepdims=True) + 1e-9)
    return x, loud


def _novelty(x: np.ndarray, w: int) -> np.ndarray:
    """Change between the `w` bars before and the `w` bars after each bar line. Averaging over
    a typical chord-progression length keeps chord changes inside a section from reading as
    section boundaries."""
    n = x.shape[1]
    nov = np.zeros(n)
    for i in range(2, n - 1):
        a = x[:, max(0, i - w):i].mean(axis=1)
        b = x[:, i:min(n, i + w)].mean(axis=1)
        d = np.linalg.norm(a) * np.linalg.norm(b)
        nov[i] = 1.0 - float(a @ b / d) if d > 1e-12 else 0.0
    return nov / (nov.max() + 1e-12)


def _segment_similarity(ssm, a, b):
    """Mean similarity of two segments aligned bar by bar, best of small shifts (a boundary
    may be a bar or two off)."""
    (a0, a1), (b0, b1) = a, b
    n = ssm.shape[0]
    best = -1.0
    for lag in (-2, -1, 0, 1, 2):
        vals = [ssm[a0 + i, b0 + i + lag] for i in range(a1 - a0)
                if 0 <= b0 + i + lag < n and b0 <= b0 + i + lag < b1 + 2]
        if len(vals) >= max(2, (min(a1 - a0, b1 - b0) * 3) // 4):
            best = max(best, float(np.mean(vals)))
    return best


def find_sections(feats: Features, bars: np.ndarray, min_bars: int = 4):
    """`bars` = bar start times (s) plus the end of the last bar. Returns
    [{"label","section","startSec","bar","confidence"}] with bar indices relative to `bars`."""
    nb = len(bars) - 1
    if nb < 2 * min_bars:
        return [{"label": "Song", "section": "custom", "startSec": round(float(bars[0]), 4), "bar": 0,
                 "confidence": 0.2}]
    x, loud = _bar_features(feats, bars)
    ssm = x.T @ x
    nov = _novelty(x, w=4)
    # Loudness steps (a band coming in or dropping out) are boundaries too, also at the edges
    # where the novelty kernel is weak.
    lnov = np.zeros(nb)
    for i in range(1, nb):
        before = loud[max(0, i - 2):i].mean()
        after = loud[i:i + 2].mean()
        lnov[i] = np.clip(abs(after - before) / 6.0, 0.0, 1.0)
    nov = 0.5 * (nov + lnov)
    # Boundaries: novelty peaks at least `min_bars` apart, strongest first.
    cand = [i for i in range(1, nb) if nov[i] >= nov[max(0, i - 1)] and nov[i] >= nov[min(nb - 1, i + 1)]]
    cand.sort(key=lambda i: -nov[i])
    thr = max(0.15, float(np.mean(nov) + 0.25 * np.std(nov)))
    picked = []
    for i in cand:
        if nov[i] < thr:
            break
        if all(abs(i - p) >= min_bars for p in picked) and i >= 2 and nb - i >= 2:
            picked.append(i)
    # Quiet starts and ends (intro / outro) are boundaries even when short.
    picked.sort()
    bounds = [0] + picked + [nb]
    segs = list(zip(bounds[:-1], bounds[1:]))

    # Group repeated segments.
    groups = []
    gid = []
    for s in segs:
        best, bi = 0.0, -1
        for g, members in enumerate(groups):
            sim = max(_segment_similarity(ssm, s, segs[m]) for m in members)
            if sim > best:
                best, bi = sim, g
        if best > 0.6:
            groups[bi].append(len(gid))
            gid.append(bi)
        else:
            groups.append([len(gid)])
            gid.append(len(groups) - 1)

    seg_loud = np.array([loud[a:b].mean() for a, b in segs])
    names = [None] * len(segs)
    repeated = [g for g in range(len(groups)) if len(groups[g]) >= 2]
    if repeated:
        chorus = max(repeated, key=lambda g: (np.mean(seg_loud[groups[g]]), len(groups[g])))
        for m in groups[chorus]:
            names[m] = "chorus"
        for g in repeated:
            if g != chorus:
                for m in groups[g]:
                    names[m] = "verse"
    quiet = seg_loud < (np.median(seg_loud) - 3.0)
    if names[0] is None or (quiet[0] and len(segs) > 2):
        names[0] = "intro"
    if len(segs) > 1 and (names[-1] is None or (quiet[-1] and len(segs) > 2)):
        names[-1] = "outro"
    for i in range(len(segs)):
        if names[i] is None:
            a, b = segs[i]
            if i + 1 < len(segs) and names[i + 1] == "chorus" and b - a <= 4:
                names[i] = "pre_chorus"
            elif "chorus" in names[:i]:
                names[i] = "bridge"
            else:
                names[i] = "verse"

    pretty = {"intro": "Intro", "verse": "Verse", "pre_chorus": "Pre-Chorus", "chorus": "Chorus",
              "bridge": "Bridge", "outro": "Outro"}
    count = {}
    multi = {n: names.count(n) for n in set(names)}
    out = []
    for i, ((a, b), n) in enumerate(zip(segs, names)):
        count[n] = count.get(n, 0) + 1
        label = pretty[n] + (f" {count[n]}" if multi[n] > 1 and n not in ("intro", "outro") else "")
        conf = 0.5 if i == 0 else float(np.clip(nov[a], 0.0, 1.0))
        out.append({"label": label, "section": n, "startSec": round(float(bars[a]), 4), "bar": int(a),
                    "bars": int(b - a), "confidence": round(conf * 0.8, 3)})
    return out
