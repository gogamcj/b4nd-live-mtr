"""Stem separation (docs/07: Demucs htdemucs_6s) with quality checks.

The separation itself is a function `separate(segment) -> (sources, names)` on stereo float
audio, so that the chunking, cross-fading, file writing and quality checks are tested without
the model (tests inject a fake). The real one is Demucs, used only when it is installed.
"""

from __future__ import annotations

import os
import wave

import numpy as np

STEM_RATE = 44100
CHUNK_SEC = 40.0      # separate in chunks: progress, and bounded memory on long songs
FADE_SEC = 2.0        # cross-fade between chunks (each chunk sees FADE_SEC of the next)
SILENT_DB = -45.0     # a stem quieter than this (vs the mix) is reported as "empty", not an error
RESIDUAL_WARN_DB = -20.0


def demucs_available() -> bool:
    try:
        import demucs  # noqa: F401
        import torch  # noqa: F401

        return True
    except ImportError:
        return False


DEMUCS_MODEL = "htdemucs_6s"
STEM_NAMES = ("drums", "bass", "other", "vocals", "guitar", "piano")   # htdemucs_6s order


def bundled_models():
    """A models folder shipped with the analyzer (htdemucs_6s.yaml + its .th file), so that a
    band PC without internet can separate: next to the standalone executable, or next to the
    package. None when there is none."""
    import sys

    here = os.path.dirname(os.path.abspath(__file__))
    for d in (os.path.join(os.path.dirname(sys.executable), "models") if getattr(sys, "frozen", False) else None,
              os.path.join(getattr(sys, "_MEIPASS", ""), "models") if getattr(sys, "_MEIPASS", None) else None,
              os.path.join(os.path.dirname(here), "models")):
        if d and os.path.isfile(os.path.join(d, DEMUCS_MODEL + ".yaml")):
            return d
    return None


def model_repo():
    return os.environ.get("B4ND_DEMUCS_REPO") or bundled_models()


def demucs_info() -> dict:
    """For `info`: whether stems can run. Does not import torch (seconds, and the MTR waits for
    `info` when it checks the analyzer)."""
    import importlib.util

    missing = [m for m in ("demucs", "torch") if importlib.util.find_spec(m) is None]
    if missing:
        return {"available": False, "reason": f"Demucs is not installed ({', '.join(missing)} missing)"}
    try:
        from importlib.metadata import version

        demucs_v, torch_v = version("demucs"), version("torch")
    except Exception:  # noqa: BLE001 - frozen builds may have no metadata
        demucs_v = torch_v = "?"
    return {"available": True, "model": DEMUCS_MODEL, "demucs": demucs_v, "torch": torch_v, "repo": model_repo()}


def fake_separator(segment: np.ndarray):
    """Stand-in for tests: splits the spectrum into 6 bands, so the stems add up to the mix
    exactly. NOT a real separation."""
    n = segment.shape[1]
    spec = np.fft.rfft(segment, axis=1)
    f = np.fft.rfftfreq(n, 1.0 / STEM_RATE)
    edges = [0, 150, 400, 1000, 2500, 6000, np.inf]
    order = ("bass", "drums", "piano", "vocals", "guitar", "other")
    out = {}
    for i, name in enumerate(order):
        m = (f >= edges[i]) & (f < edges[i + 1])
        out[name] = np.fft.irfft(spec * m, n=n, axis=1)
    return np.stack([out[k] for k in STEM_NAMES]).astype(np.float32), list(STEM_NAMES)


def demucs_separator(model_name: str = DEMUCS_MODEL, device: str = "cpu"):
    """Returns (separate_fn, names, versions). Raises ImportError / RuntimeError when the model
    cannot be loaded (not installed, weights not downloaded and no internet).
    B4ND_DEMUCS_REPO: a folder with the model files, for offline machines."""
    import pathlib

    import demucs
    import torch
    from demucs.apply import apply_model
    from demucs.pretrained import get_model

    repo = model_repo()
    try:
        model = get_model(model_name, repo=pathlib.Path(repo)) if repo else get_model(model_name)
    except Exception as e:  # noqa: BLE001 - usually no internet for the first download
        raise RuntimeError(f"cannot load the Demucs model {model_name}: {e} "
                           "(the first run downloads it; offline machines need the bundled models folder or B4ND_DEMUCS_REPO)") from e
    model.eval()
    if device == "cuda" and not torch.cuda.is_available():
        device = "cpu"
    names = list(model.sources)

    def separate(segment: np.ndarray):
        x = torch.from_numpy(np.ascontiguousarray(segment, dtype=np.float32))
        ref = x.mean(0)
        mean, std = ref.mean(), ref.std() + 1e-8
        with torch.no_grad():
            out = apply_model(model, ((x - mean) / std)[None], device=device, shifts=1, split=True, overlap=0.25,
                              progress=False)[0]
        out = out * std + mean
        return out.cpu().numpy(), names

    return separate, names, {"demucs": getattr(demucs, "__version__", "?"), "torch": torch.__version__,
                             "model": model_name, "device": device, "modelSource": "bundled" if repo else "download cache"}


def load_stereo(path: str, sr: int = STEM_RATE) -> np.ndarray:
    """2 x n float32 at `sr` (mono files are duplicated)."""
    from math import gcd

    from scipy import signal

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
    x = data.T
    if x.shape[0] == 1:
        x = np.vstack([x, x])
    x = x[:2]
    if fs != sr:
        g = gcd(int(fs), int(sr))
        x = signal.resample_poly(x, sr // g, int(fs) // g, axis=1)
    return np.nan_to_num(x.astype(np.float32))


def separate_chunked(mix: np.ndarray, separate, sr: int = STEM_RATE, progress=lambda p: None):
    """Runs `separate` on overlapping chunks and cross-fades them. Returns (stems, names) with
    stems shaped (n_stems, 2, n)."""
    n = mix.shape[1]
    chunk = int(CHUNK_SEC * sr)
    fade = int(FADE_SEC * sr)
    out = None
    names = None
    starts = list(range(0, max(1, n), chunk))
    for k, s in enumerate(starts):
        e = min(n, s + chunk + fade)
        src, nm = separate(mix[:, s:e])
        src = np.asarray(src, dtype=np.float32)
        if out is None:
            names = list(nm)
            out = np.zeros((len(names), 2, n), dtype=np.float32)
        seg = e - s
        w = np.ones(seg, dtype=np.float32)
        if k > 0:   # fade in over the overlap written by the previous chunk
            m = min(fade, seg)
            w[:m] = np.linspace(0.0, 1.0, m, dtype=np.float32)
        if e < n:   # fade out over the part the next chunk also covers
            m = min(fade, seg)
            w[seg - m:] *= np.linspace(1.0, 0.0, m, dtype=np.float32)
        out[:, :, s:e] += src[:, :, :seg] * w
        progress((k + 1) / len(starts))
    return out, names


def _db(x: float) -> float:
    return float(20.0 * np.log10(max(x, 1e-12)))


def quality(mix: np.ndarray, stems: np.ndarray, names) -> dict:
    """Energy of each stem against the mix, and how much of the mix the stems do not add up to
    (the residual). A high residual means the separation lost or invented sound."""
    mix_rms = float(np.sqrt(np.mean(np.square(mix)))) + 1e-12
    resid = mix - stems.sum(axis=0)
    residual_db = _db(float(np.sqrt(np.mean(np.square(resid)))) / mix_rms)
    files = []
    for i, name in enumerate(names):
        rms = float(np.sqrt(np.mean(np.square(stems[i]))))
        energy_db = _db(rms / mix_rms)
        files.append({"name": name, "energyDb": round(energy_db, 1), "empty": energy_db < SILENT_DB})
    warnings = []
    if residual_db > RESIDUAL_WARN_DB:
        warnings.append(f"stems do not add up to the mix (residual {residual_db:.1f} dB)")
    return {"residualDb": round(residual_db, 1), "files": files, "warnings": warnings}


def write_wav24(path: str, x: np.ndarray, sr: int):
    """2 x n float -> 24-bit PCM WAV (clipped; separation can overshoot slightly)."""
    peak = float(np.max(np.abs(x))) if x.size else 0.0
    v = np.clip(np.round(x.T * 8388607.0), -8388608, 8388607).astype(np.int32)
    b = np.stack([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF], axis=-1).astype(np.uint8)
    tmp = path + ".tmp"
    with wave.open(tmp, "wb") as w:
        w.setnchannels(x.shape[0])
        w.setsampwidth(3)
        w.setframerate(sr)
        w.writeframes(b.tobytes())
    os.replace(tmp, path)
    return peak


def run_stems(input_path: str, out_dir: str, separate=None, names_versions=None, progress=lambda p: None,
              device: str = "cpu") -> dict:
    """Separates `input_path` into out_dir/stems/<name>.wav. Returns the result["stems"] dict."""
    versions = {}
    if separate is None:
        if not demucs_available():
            raise RuntimeError("Demucs is not installed (pip install demucs) - stems need it")
        separate, _, versions = demucs_separator(device=device)
    elif names_versions:
        versions = names_versions
    progress(0.05)
    mix = load_stereo(input_path)
    progress(0.1)
    stems, names = separate_chunked(mix, separate, progress=lambda p: progress(0.1 + 0.8 * p))
    q = quality(mix, stems, names)
    stem_dir = os.path.join(out_dir, "stems")
    os.makedirs(stem_dir, exist_ok=True)
    for i, f in enumerate(q["files"]):
        rel = f"stems/{f['name']}.wav"
        peak = write_wav24(os.path.join(out_dir, rel), stems[i], STEM_RATE)
        f["path"] = rel
        f["clipped"] = peak > 1.0
        if f["clipped"]:
            q["warnings"].append(f"{f['name']}: peaks over 0 dBFS were clipped")
    progress(1.0)
    q.update({"sampleRate": STEM_RATE, "versions": versions, "model": versions.get("model", "custom")})
    return q
