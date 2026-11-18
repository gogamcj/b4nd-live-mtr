"""Audio to MIDI (docs/07 §2 step 6), per stem.

- piano / guitar / other: Basic Pitch (Spotify, Apache-2.0) polyphonic notes. The model
  (models/basic_pitch/nmp.onnx, 230 KB) runs with onnxruntime; windowing and note decoding follow
  basic_pitch/inference.py and note_creation.py, without librosa / tensorflow.
- bass / vocals: the same model limited to the instrument's range, then reduced to one note at
  a time (docs/07 names torchcrepe for this; one model is easier to ship and verify - see §8).
- drums: onsets in three bands -> GM kick 36 / snare 38 / closed hi-hat 42 (own, no model).

Notes are (start s, end s, MIDI pitch, velocity 1..127, confidence 0..1) in the input file's time.
"""

from __future__ import annotations

import math
import os

import numpy as np
from scipy import signal

BP_SR = 22050
BP_HOP = 256
BP_WIN = BP_SR * 2 - BP_HOP          # 43844 samples per model window
BP_FRAMES = 172                      # model frames per window
BP_OVERLAP_FRAMES = 30
BP_OVERLAP = BP_OVERLAP_FRAMES * BP_HOP
BP_STEP = BP_WIN - BP_OVERLAP        # 36164
BP_KEEP = BP_FRAMES - BP_OVERLAP_FRAMES   # frames kept per window after trimming both overlaps
MIDI_OFFSET = 21                     # model bin 0 = A0
N_PITCHES = 88

GM_KICK, GM_SNARE, GM_HAT = 36, 38, 42

# What each stem becomes (docs/07). Unknown names are treated as polyphonic.
PROFILES = {
    "bass":   {"mode": "mono", "min_hz": 28.0, "max_hz": 420.0, "program": 33},    # Electric Bass (finger)
    "vocals": {"mode": "mono", "min_hz": 75.0, "max_hz": 1100.0, "program": 53},   # Voice Oohs
    "piano":  {"mode": "poly", "min_hz": 27.5, "max_hz": 4200.0, "program": 0},
    "guitar": {"mode": "poly", "min_hz": 75.0, "max_hz": 1600.0, "program": 27},   # Electric Guitar (clean)
    "other":  {"mode": "poly", "min_hz": 40.0, "max_hz": 4200.0, "program": 48},   # Strings
    "drums":  {"mode": "drums", "program": 0},
}


def model_path() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "models", "basic_pitch", "nmp.onnx")


def onnx_available() -> bool:
    import importlib.util

    return importlib.util.find_spec("onnxruntime") is not None and os.path.isfile(model_path())


def load_mono(path: str, sr: int) -> np.ndarray:
    from math import gcd

    from .audio import _read_wav

    data = None
    if not os.environ.get("B4ND_ANALYZER_NO_SOUNDFILE"):
        try:
            import soundfile

            data, fs = soundfile.read(path, dtype="float32", always_2d=True)
        except ImportError:
            pass
    if data is None:
        data, fs = _read_wav(path)
    y = data.mean(axis=1).astype(np.float32)
    if fs != sr:
        g = gcd(int(fs), int(sr))
        y = signal.resample_poly(y, sr // g, int(fs) // g).astype(np.float32)
    return np.nan_to_num(y)


# --- Basic Pitch -----------------------------------------------------------------------------

class BasicPitch:
    def __init__(self):
        import onnxruntime as ort

        opts = ort.SessionOptions()
        opts.intra_op_num_threads = max(1, min(4, (os.cpu_count() or 2) - 1))
        self.session = ort.InferenceSession(model_path(), sess_options=opts, providers=["CPUExecutionProvider"])
        self.version = ort.__version__

    def posteriorgrams(self, y: np.ndarray, progress=lambda p: None):
        """(note, onset) activations, (n_frames, 88), frame n at frame_time(n)."""
        n_frames = int(math.floor(len(y) * (BP_SR // BP_HOP) / BP_SR))
        padded = np.concatenate([np.zeros(BP_OVERLAP // 2, np.float32), y.astype(np.float32)])
        starts = list(range(0, len(padded), BP_STEP)) or [0]
        notes, onsets = [], []
        trim = BP_OVERLAP_FRAMES // 2
        batch = 8
        for b in range(0, len(starts), batch):
            wins = []
            for s in starts[b:b + batch]:
                w = padded[s:s + BP_WIN]
                if len(w) < BP_WIN:
                    w = np.pad(w, (0, BP_WIN - len(w)))
                wins.append(w)
            x = np.stack(wins)[:, :, None].astype(np.float32)
            note, onset = self.session.run(["StatefulPartitionedCall:1", "StatefulPartitionedCall:2"],
                                           {"serving_default_input_2:0": x})
            notes.append(note[:, trim:-trim, :])
            onsets.append(onset[:, trim:-trim, :])
            progress(min(1.0, (b + batch) / len(starts)))
        note = np.concatenate(notes).reshape(-1, N_PITCHES)[:n_frames]
        onset = np.concatenate(onsets).reshape(-1, N_PITCHES)[:n_frames]
        return note, onset


def frame_time(n) -> np.ndarray:
    """Time (s, input time) of global output frame n: window n // BP_KEEP, frame BP_KEEP-trimmed.
    Exact for the windowing above (Basic Pitch itself approximates the drift per window)."""
    n = np.asarray(n)
    w = n // BP_KEEP
    j = n % BP_KEEP
    return (w * BP_STEP - BP_OVERLAP // 2 + (BP_OVERLAP_FRAMES // 2 + j) * BP_HOP) / BP_SR


def _hz_to_bin(hz: float) -> int:
    return int(round(12.0 * math.log2(hz / 440.0) + 69.0 - MIDI_OFFSET))


def decode_notes(frames: np.ndarray, onsets: np.ndarray, onset_thresh=0.5, frame_thresh=0.3, min_note_ms=127.7,
                 min_hz=None, max_hz=None, energy_tol=11, melodia=True):
    """Basic Pitch note decoding (note_creation.output_to_notes_polyphonic). Returns
    [(start_frame, end_frame, pitch, amplitude, model onset activation at the start)]."""
    frames = frames.astype(np.float64).copy()
    onsets = onsets.astype(np.float64).copy()
    model_onsets = onsets.copy()
    n_frames = frames.shape[0]
    if n_frames < 3:
        return []
    min_len = int(round(min_note_ms / 1000.0 * (BP_SR / BP_HOP)))
    if max_hz is not None:
        k = max(0, min(N_PITCHES, _hz_to_bin(max_hz)))
        frames[:, k:] = 0
        onsets[:, k:] = 0
    if min_hz is not None:
        k = max(0, min(N_PITCHES, _hz_to_bin(min_hz)))
        frames[:, :k] = 0
        onsets[:, :k] = 0
    # Onsets inferred from frame jumps, scaled to the onset maximum.
    diffs = []
    for n in (1, 2):
        app = np.concatenate([np.zeros((n, frames.shape[1])), frames])
        diffs.append(app[n:, :] - app[:-n, :])
    fd = np.min(diffs, axis=0)
    fd[fd < 0] = 0
    fd[:2, :] = 0
    if fd.max() > 0:
        fd = onsets.max() * fd / fd.max()
    onsets = np.maximum(onsets, fd)

    peaks = np.zeros_like(onsets)
    idx = signal.argrelmax(onsets, axis=0)
    peaks[idx] = onsets[idx]
    t_idx, f_idx = np.where(peaks >= onset_thresh)
    order = np.argsort(-t_idx, kind="stable")   # backwards in time
    remaining = frames.copy()
    events = []
    last = N_PITCHES - 1

    def clear(a, b, f):
        remaining[a:b, f] = 0
        if f < last:
            remaining[a:b, f + 1] = 0
        if f > 0:
            remaining[a:b, f - 1] = 0

    for t0, f in zip(t_idx[order], f_idx[order]):
        if t0 >= n_frames - 1:
            continue
        i, k = t0 + 1, 0
        while i < n_frames - 1 and k < energy_tol:
            k = k + 1 if remaining[i, f] < frame_thresh else 0
            i += 1
        i -= k
        if i - t0 <= min_len:
            continue
        clear(t0, i, f)
        events.append((int(t0), int(i), int(f) + MIDI_OFFSET, float(np.mean(frames[t0:i, f])),
                       float(model_onsets[max(0, t0 - 1):t0 + 2, f].max())))

    if melodia:
        guard = 0
        while remaining.max() > frame_thresh and guard < 20000:
            guard += 1
            i_mid, f = np.unravel_index(np.argmax(remaining), remaining.shape)
            remaining[i_mid, f] = 0
            i, k = i_mid + 1, 0
            while i < n_frames - 1 and k < energy_tol:
                k = k + 1 if remaining[i, f] < frame_thresh else 0
                clear(i, i + 1, f)
                i += 1
            i_end = i - 1 - k
            i, k = i_mid - 1, 0
            while i > 0 and k < energy_tol:
                k = k + 1 if remaining[i, f] < frame_thresh else 0
                clear(i, i + 1, f)
                i -= 1
            i_start = i + 1 + k
            if i_end - i_start <= min_len:
                continue
            events.append((int(i_start), int(i_end), int(f) + MIDI_OFFSET, float(np.mean(frames[i_start:i_end, f])),
                           float(model_onsets[max(0, i_start - 1):i_start + 2, f].max())))
    return events


def merge_retriggers(events, onset_thresh=0.7):
    """A note that continues one of the same pitch without a gap, and whose start the model did
    see only as a weak onset (re-attacks measure ~0.6, real attacks ~0.9), is the same note re-triggered by another note's attack: merged."""
    out = []
    last = {}
    for ev in sorted(events, key=lambda e: (e[0], e[2])):
        s, e, p, a, o = ev
        j = last.get(p)
        if j is not None and s - out[j][1] <= 1 and o < onset_thresh:
            ps, pe, _, pa, po = out[j]
            out[j] = (ps, max(pe, e), p, (pa * (pe - ps) + a * (e - s)) / max(1, max(pe, e) - ps), po)
            continue
        last[p] = len(out)
        out.append(ev)
    return out


# Decoded onsets come out about one frame early (measured on synthetic bass and piano notes).
BP_LAG = 0.011


def to_notes(events):
    out = []
    for s, e, p, a, *_ in events:
        t0, t1 = float(frame_time(s)) + BP_LAG, float(frame_time(e)) + BP_LAG
        if t1 <= t0:
            continue
        out.append([round(max(0.0, t0), 4), round(t1, 4), int(p), int(np.clip(round(127 * a), 20, 127)),
                    round(float(np.clip(a, 0.0, 1.0)), 3)])
    out.sort(key=lambda n: (n[0], n[2]))
    return out


def clean_poly(notes, min_conf=0.35):
    """Weak notes in polyphonic material are mostly harmonics of stronger ones."""
    return [n for n in notes if n[4] >= min_conf]


def monophonic(notes):
    """One note at a time: where notes overlap the stronger one wins and the earlier one is cut
    at the later one's start (bass lines and melodies)."""
    out = []
    for n in sorted(notes, key=lambda n: (n[0], -n[4])):
        if out and n[0] < out[-1][1] - 1e-6:
            prev = out[-1]
            if n[4] > prev[4] or n[0] - prev[0] > 0.08:
                if n[0] - prev[0] < 0.03:   # nearly the same start: keep only the stronger
                    out[-1] = n
                    continue
                prev[1] = n[0]
                out.append(n)
            # else: a weaker note inside the current one (octave / harmonic): dropped
            continue
        out.append(n)
    return [n for n in out if n[1] - n[0] >= 0.05]


# --- drums -----------------------------------------------------------------------------------

DR_SR = 44100
DR_NFFT = 1024
DR_HOP = 256


def drum_notes(y: np.ndarray, sr: int = DR_SR):
    """Kick / snare / hi-hat hits from band onsets. Returns notes like the others (fixed 60 ms
    length) and the band onset functions' frame rate for tests."""
    if len(y) < DR_NFFT * 2:
        return []
    f, t, z = signal.stft(y, fs=sr, nperseg=DR_NFFT, noverlap=DR_NFFT - DR_HOP, boundary=None, padded=False)
    mag = np.abs(z)
    bands = {"low": (30, 140), "mid": (180, 1200), "noise": (2500, 6000), "high": (7000, 16000)}
    env = {}
    for k, (a, b) in bands.items():
        e = np.log1p(1000.0 * mag[(f >= a) & (f < b)].mean(axis=0))
        flux = np.maximum(0.0, np.diff(e, prepend=e[:1]))
        env[k] = (e, flux)
    times = t   # centre of each frame (scipy pads nothing: frame k centred at k*hop + nfft/2)

    def picks(flux, min_gap_s=0.06, k_sigma=2.5):
        if flux.max() <= 0:
            return [], 0.0
        med = signal.medfilt(flux, 31)
        mad = signal.medfilt(np.abs(flux - med), 31) + 1e-6
        thr = med + k_sigma * mad + 0.1 * flux.max()
        cand = signal.argrelmax(flux)[0]
        out = []
        for c in cand:
            if flux[c] < thr[c]:
                continue
            if out and times[c] - times[out[-1]] < min_gap_s:
                if flux[c] > flux[out[-1]]:
                    out[-1] = c
                continue
            out.append(c)
        return out, float(flux.max())

    hits = []
    low_p, low_max = picks(env["low"][1])
    mid_p, mid_max = picks(env["mid"][1])
    noi_p, _ = picks(env["noise"][1])
    hi_p, hi_max = picks(env["high"][1], min_gap_s=0.04)
    noise_set = set(noi_p)

    def near(c, s, d=3):
        return any(abs(c - x) <= d for x in s)

    noise_max = env["noise"][1].max() + 1e-9
    for c in low_p:
        low_n = env["low"][1][c] / low_max
        # The snare's noise reaches the low band too: a kick is a hit where the low band leads.
        noise_n = env["noise"][1][max(0, c - 2):c + 3].max() / noise_max
        if low_n < 0.3 or (noise_n > 0.3 and low_n < 1.3 * noise_n):
            continue
        hits.append((c, GM_KICK, low_n))
    for c in mid_p:
        # Snare: mid-band hit with broadband noise, not just the kick's upper tail.
        if near(c, noise_set) and env["mid"][1][c] / mid_max > 0.25:
            if near(c, low_p) and env["noise"][1][c] < 0.5 * env["noise"][1].max():
                continue
            hits.append((c, GM_SNARE, env["mid"][1][c] / mid_max))
    for c in hi_p:
        hits.append((c, GM_HAT, env["high"][1][c] / hi_max))
    notes = []
    # The STFT frame centre lags the hit by about half a frame of rise: measured 15 ms on synthetic hits.
    lag = 0.015
    for c, pitch, s in sorted(hits):
        t0 = max(0.0, float(times[c]) - DR_NFFT / 2 / sr + lag)
        s = float(np.clip(s, 0.0, 1.0))
        notes.append([round(t0, 4), round(t0 + 0.06, 4), pitch, int(np.clip(round(40 + 87 * s), 1, 127)),
                      round(min(1.0, 0.4 + 0.6 * s), 3)])
    return notes


# --- per stem --------------------------------------------------------------------------------

def transcribe_file(path: str, name: str, model=None, progress=lambda p: None) -> dict:
    prof = PROFILES.get(name, PROFILES["other"])
    if prof["mode"] == "drums":
        y = load_mono(path, DR_SR)
        notes = drum_notes(y)
        backend = "b4nd-drums"
    else:
        if model is None:
            raise RuntimeError("onnxruntime is not installed (pip install onnxruntime) - needed for pitched instruments")
        y = load_mono(path, BP_SR)
        frames, onsets = model.posteriorgrams(y, progress=lambda p: progress(0.9 * p))
        ev = decode_notes(frames, onsets, min_hz=prof["min_hz"], max_hz=prof["max_hz"],
                          min_note_ms=90.0 if prof["mode"] == "mono" else 127.7)
        notes = to_notes(merge_retriggers(ev))
        notes = monophonic(notes) if prof["mode"] == "mono" else clean_poly(notes)
        backend = "basic-pitch-onnx"
    progress(1.0)
    conf = float(np.mean([n[4] for n in notes])) if notes else 0.0
    return {"name": name, "mode": prof["mode"], "program": prof["program"], "drums": prof["mode"] == "drums",
            "backend": backend, "notes": notes, "noteCount": len(notes), "confidence": round(conf, 3),
            "durationSec": round(len(y) / (DR_SR if prof["mode"] == "drums" else BP_SR), 3)}


def run_midi(inputs, progress=lambda p: None, log=lambda m: None):
    """inputs: [{"name": "bass", "path": ".../bass.wav"}]. Returns (result["midi"], errors)."""
    model = None
    errors = []
    versions = {}
    if any(PROFILES.get(i.get("name"), PROFILES["other"])["mode"] != "drums" for i in inputs):
        if onnx_available():
            model = BasicPitch()
            versions = {"onnxruntime": model.version, "basicPitchModel": "icassp_2022"}
        else:
            errors.append("midi: onnxruntime is not installed - only drums were converted")
    tracks = []
    for k, inp in enumerate(inputs):
        name = str(inp.get("name", "other"))
        try:
            if PROFILES.get(name, PROFILES["other"])["mode"] != "drums" and model is None:
                continue
            r = transcribe_file(inp["path"], name, model, progress=lambda p, k=k: progress((k + p) / len(inputs)))
            r["sourceId"] = str(inp.get("id", ""))[:64]
            tracks.append(r)
        except Exception as e:  # noqa: BLE001 - one stem failing does not stop the others
            errors.append(f"midi {name}: {e}")
            import traceback

            log(traceback.format_exc())
    progress(1.0)
    return {"tracks": tracks, "versions": versions}, errors
