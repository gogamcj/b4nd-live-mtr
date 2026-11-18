"""Key (Krumhansl-Kessler profiles) and chords (triad templates + Viterbi), docs/07 §1."""

from __future__ import annotations

import numpy as np

from .audio import CHROMA_HOP, frame_time, sync

SHARP = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
FLAT = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
MAJOR = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
MINOR = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
# Keys written with flats: F Bb Eb Ab Db Gb major, D G C F Bb Eb minor.
FLAT_MAJOR = {5, 10, 3, 8, 1, 6}
FLAT_MINOR = {2, 7, 0, 5, 10, 3}


def note_names(tonic: int, minor: bool):
    return FLAT if (tonic in (FLAT_MINOR if minor else FLAT_MAJOR)) else SHARP


def key_name(tonic: int, minor: bool) -> str:
    return f"{note_names(tonic, minor)[tonic]} {'minor' if minor else 'major'}"


def _corr(a, b):
    a = a - a.mean()
    b = b - b.mean()
    d = np.linalg.norm(a) * np.linalg.norm(b)
    return float(a @ b / d) if d > 1e-12 else 0.0


def estimate_key(chroma: np.ndarray, weights: np.ndarray | None = None):
    """Returns (tonic, minor, confidence, candidates[{"value","score"}])."""
    w = np.ones(chroma.shape[1]) if weights is None else weights
    prof = (chroma * w[None, :]).sum(axis=1)
    scores = []
    for t in range(12):
        scores.append((_corr(prof, np.roll(MAJOR, t)), t, False))
        scores.append((_corr(prof, np.roll(MINOR, t)), t, True))
    scores.sort(key=lambda s: -s[0])
    r1, r2 = scores[0][0], scores[1][0]
    conf = float(np.clip((r1 - r2) / 0.15, 0.0, 1.0) * 0.6 + np.clip(r1, 0.0, 1.0) * 0.4)
    # Six candidates: the chord evidence (refine_key_with_chords) may promote one of them.
    cands = [{"value": key_name(t, m), "score": round(float(r), 3)} for (r, t, m) in scores[:6]]
    return scores[0][1], scores[0][2], round(conf, 3), cands


def _templates():
    names, temps = [], []
    for minor in (False, True):
        for r in range(12):
            v = np.zeros(12)
            v[[r, (r + (3 if minor else 4)) % 12, (r + 7) % 12]] = 1.0
            temps.append(v / np.linalg.norm(v))
            names.append((r, minor))
    return names, np.array(temps)


def estimate_chords(chroma: np.ndarray, bounds: np.ndarray, energy: np.ndarray, tonic: int, minor_key: bool,
                    stay: float = 0.85, beta: float = 18.0):
    """Beat-synchronous chord labels. `bounds` are beat times (s); `energy` per beat (0..1
    relative). Returns [{"startSec","endSec","chord","confidence"}]."""
    if len(bounds) < 2:
        return []
    ct = frame_time(np.arange(chroma.shape[1]), hop=CHROMA_HOP)
    c = sync(chroma, ct, bounds)
    c = c - c.min(axis=0, keepdims=True)
    c = c / (np.linalg.norm(c, axis=0, keepdims=True) + 1e-9)
    names, temps = _templates()
    sim = temps @ c                                   # 24 x beats
    quiet = energy < 0.05
    # "N" (no chord): fixed similarity, wins on silence and noise.
    n_sim = np.where(quiet, 1.0, 0.55)
    sim = np.vstack([sim, n_sim[None, :]])
    em = np.exp(beta * (sim - sim.max(axis=0, keepdims=True)))
    em = em / em.sum(axis=0, keepdims=True)
    ns = sim.shape[0]
    logT = np.full((ns, ns), np.log((1.0 - stay) / (ns - 1)))
    np.fill_diagonal(logT, np.log(stay))
    logE = np.log(em + 1e-12)
    T = logE.shape[1]
    dp = np.zeros((ns, T))
    bp = np.zeros((ns, T), dtype=int)
    dp[:, 0] = logE[:, 0]
    for t in range(1, T):
        m = dp[:, t - 1][:, None] + logT
        bp[:, t] = np.argmax(m, axis=0)
        dp[:, t] = m[bp[:, t], np.arange(ns)] + logE[:, t]
    path = np.zeros(T, dtype=int)
    path[-1] = int(np.argmax(dp[:, -1]))
    for t in range(T - 1, 0, -1):
        path[t - 1] = bp[path[t], t]
    spell = note_names(tonic, minor_key)
    out = []
    for t in range(T):
        s = path[t]
        label = "N" if s == 24 else spell[names[s][0]] + ("m" if names[s][1] else "")
        p = float(em[s, t])
        if out and out[-1]["chord"] == label:
            out[-1]["endSec"] = float(bounds[t + 1])
            out[-1]["_p"].append(p)
        else:
            out.append({"startSec": float(bounds[t]), "endSec": float(bounds[t + 1]), "chord": label, "_p": [p]})
    for o in out:
        o["confidence"] = round(float(np.mean(o.pop("_p"))), 3)
        o["startSec"] = round(o["startSec"], 4)
        o["endSec"] = round(o["endSec"], 4)
    return out


def parse_key(name: str):
    note, mode = name.split()
    tonic = SHARP.index(note) if note in SHARP else FLAT.index(note)
    return tonic, mode == "minor"


def chord_root(label: str):
    if label == "N":
        return None
    minor = label.endswith("m")
    n = label[:-1] if minor else label
    return (SHARP.index(n) if n in SHARP else FLAT.index(n)), minor


def refine_key_with_chords(key: dict, chords: list) -> dict:
    """A major key and its relative minor share one scale, so the pitch profile alone often
    cannot tell them apart. Decide between the two by the tonic chords: how long each sounds,
    and whether the song starts and ends on it."""
    if not chords or not key.get("candidates"):
        return key
    played = [c for c in chords if c["chord"] != "N"]
    if not played:
        return key
    total = sum(c["endSec"] - c["startSec"] for c in played)

    def evidence(t, m):
        share = sum(c["endSec"] - c["startSec"] for c in played if chord_root(c["chord"]) == (t, m)) / total
        return share + 0.25 * (chord_root(played[0]["chord"]) == (t, m)) + 0.25 * (chord_root(played[-1]["chord"]) == (t, m))

    top = key["candidates"][0]
    t, m = parse_key(top["value"])
    rel = ((t + 9) % 12, True) if not m else ((t + 3) % 12, False)
    if evidence(*rel) <= evidence(t, m) + 0.1:
        return key
    rel_name = key_name(*rel)
    score = next((c["score"] for c in key["candidates"] if c["value"] == rel_name), top["score"])
    cands = [{"value": rel_name, "score": score}] + [c for c in key["candidates"] if c["value"] != rel_name]
    out = dict(key)
    out["value"] = rel_name
    # Chosen by chord evidence against a near-equal profile: never report high confidence.
    out["confidence"] = round(min(0.7, max(0.3, float(key["confidence"]))), 3)
    out["candidates"] = cands
    out["decidedBy"] = "tonic chord"
    return out
