"""Audio loading and the shared time-frequency features."""

from __future__ import annotations

import math
import os
import wave
from dataclasses import dataclass

import numpy as np
from scipy import ndimage, signal

SR = 22050          # analysis sample rate
HOP = 256           # onset / beat frame hop (11.6 ms)
NFFT = 1024
CHROMA_HOP = 2048   # chroma frame hop (92.9 ms)
CHROMA_NFFT = 8192  # long window: 2.7 Hz bins resolve semitones down to C2


def load_audio(path: str, sr: int = SR) -> np.ndarray:
    """Mono float32 at `sr`. Uses soundfile when installed, else the WAV readers."""
    data = None
    fs = 0
    if not os.environ.get("B4ND_ANALYZER_NO_SOUNDFILE"):
        try:
            import soundfile  # optional

            data, fs = soundfile.read(path, dtype="float32", always_2d=True)
        except ImportError:
            pass
    if data is None:
        data, fs = _read_wav(path)
    if data.size == 0:
        raise ValueError("the audio file is empty")
    mono = data.mean(axis=1).astype(np.float32)
    if fs != sr:
        g = math.gcd(int(fs), int(sr))
        mono = signal.resample_poly(mono, sr // g, int(fs) // g).astype(np.float32)
    if not np.all(np.isfinite(mono)):
        mono = np.nan_to_num(mono)
    return mono


def _read_wav(path: str):
    try:
        from scipy.io import wavfile

        fs, x = wavfile.read(path)
    except ValueError:
        # 24-bit WAV on old scipy: fall back to the stdlib reader.
        with wave.open(path, "rb") as w:
            fs = w.getframerate()
            ch = w.getnchannels()
            width = w.getsampwidth()
            raw = w.readframes(w.getnframes())
        if width != 3:
            raise
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = (v / float(1 << 23)).reshape(-1, ch)
    if x.ndim == 1:
        x = x[:, None]
    if x.dtype.kind == "i":
        x = x.astype(np.float32) / float(np.iinfo(x.dtype).max)
    elif x.dtype.kind == "u":
        x = (x.astype(np.float32) - 128.0) / 128.0
    return x.astype(np.float32), int(fs)


# The log spectral flux peaks while a hit is still entering the window: 12 ms before the
# hit at NFFT 1024 (measured on clicks and synthetic drums, tests/test_tempo.py).
ONSET_LAG = 0.012


def frame_time(frames, hop: int = HOP, sr: int = SR) -> np.ndarray:
    """Time (s) at the centre of each frame of a centred STFT."""
    return np.asarray(frames, dtype=np.float64) * hop / sr


def onset_time(frames) -> np.ndarray:
    """Time (s) of the hit that makes the onset function peak at `frames`."""
    return frame_time(frames) + ONSET_LAG


def stft_mag(y: np.ndarray, n_fft: int, hop: int) -> np.ndarray:
    """|STFT| (bins x frames), centred frames (frame k is centred on sample k*hop)."""
    pad = n_fft // 2
    yp = np.pad(y, (pad, pad))
    n = 1 + max(0, (len(yp) - n_fft) // hop)
    win = signal.get_window("hann", n_fft).astype(np.float32)
    out = np.empty((n_fft // 2 + 1, n), dtype=np.float32)
    block = 512
    for s in range(0, n, block):
        e = min(n, s + block)
        idx = np.arange(s, e)[:, None] * hop + np.arange(n_fft)[None, :]
        out[:, s:e] = np.abs(np.fft.rfft(yp[idx] * win, axis=1)).T
    return out


@dataclass
class Features:
    duration: float
    onset: np.ndarray        # broadband onset strength, one value per HOP frame
    onset_low: np.ndarray    # < 150 Hz (kick) onset strength
    rms: np.ndarray          # frame RMS (HOP frames)
    bands: np.ndarray        # 12 x frames log band energies (timbre), HOP frames
    chroma: np.ndarray       # 12 x frames, CHROMA_HOP frames, harmonic part only

    @property
    def fps(self) -> float:
        return SR / HOP


def compute_features(y: np.ndarray, progress=None) -> Features:
    duration = len(y) / SR
    mag = stft_mag(y, NFFT, HOP)
    freqs = np.fft.rfftfreq(NFFT, 1.0 / SR)
    if progress:
        progress(0.3)

    logm = np.log1p(100.0 * mag)
    flux = np.maximum(0.0, np.diff(logm, axis=1, prepend=logm[:, :1]))
    keep = freqs < 8000.0
    low = freqs < 150.0
    onset = flux[keep].mean(axis=0)
    onset_low = flux[low].mean(axis=0)

    frames = mag.shape[1]
    rms = np.sqrt(np.mean(np.square(mag), axis=0)) * (2.0 / NFFT)

    edges = np.geomspace(40.0, 10000.0, 13)
    bands = np.zeros((12, frames), dtype=np.float32)
    for i in range(12):
        sel = (freqs >= edges[i]) & (freqs < edges[i + 1])
        bands[i] = np.log(1e-6 + np.mean(np.square(mag[sel]), axis=0))
    del mag, logm, flux
    if progress:
        progress(0.6)

    chroma = _chroma(y)
    if progress:
        progress(1.0)
    return Features(duration, onset.astype(np.float32), onset_low.astype(np.float32),
                    rms.astype(np.float32), bands, chroma)


def _chroma(y: np.ndarray) -> np.ndarray:
    """Pitch-class energy from spectral peaks between 55 Hz and 1.76 kHz.

    Only local spectral maxima count (window leakage would smear every note over its
    neighbours), each assigned to its nearest semitone with a weight that falls off with
    the distance from the equal-tempered pitch. Higher partials are de-emphasised so that
    the 3rd and 5th harmonics do not read as extra fifths and thirds."""
    mag = stft_mag(y, CHROMA_NFFT, CHROMA_HOP)
    freqs = np.fft.rfftfreq(CHROMA_NFFT, 1.0 / SR)
    sel = (freqs >= 50.0) & (freqs <= 1800.0)
    m = mag[sel]
    f = freqs[sel]
    # Harmonic part: a median filter along time removes drum hits (short in time).
    m = ndimage.median_filter(m, size=(1, 9), mode="nearest")
    peak = np.zeros_like(m, dtype=bool)
    peak[1:-1] = (m[1:-1] > m[:-2]) & (m[1:-1] >= m[2:])
    floor = m.max(axis=0, keepdims=True) * 10 ** (-50 / 20)
    peak &= m > floor
    # Parabolic interpolation of each peak's frequency (log magnitude).
    lm = np.log(m + 1e-9)
    k = np.arange(m.shape[0])[:, None]
    up = np.vstack([lm[1:], lm[-1:]])
    dn = np.vstack([lm[:1], lm[:-1]])
    den = dn - 2 * lm + up
    safe = np.where(np.abs(den) > 1e-9, den, 1.0)
    frac = np.where(np.abs(den) > 1e-9, 0.5 * (dn - up) / safe, 0.0)
    df = f[1] - f[0]
    fpk = np.clip(f[0] + (k + np.clip(frac, -0.5, 0.5)) * df, 1.0, None)
    pitch = 69.0 + 12.0 * np.log2(fpk / 440.0)
    nearest = np.round(pitch)
    dev = np.abs(pitch - nearest)
    w = np.where(peak, np.cos(np.minimum(dev, 0.5) * np.pi) ** 2, 0.0)
    w *= np.sqrt(np.clip(220.0 / fpk, 0.0, 1.0)) + 0.25   # less weight to high partials
    e = np.sqrt(m) * w
    pc = (nearest.astype(int) % 12)
    chroma = np.zeros((12, m.shape[1]), dtype=np.float32)
    for c in range(12):
        chroma[c] = np.sum(np.where(pc == c, e, 0.0), axis=0)
    return chroma


def sync(features: np.ndarray, frame_times: np.ndarray, bounds: np.ndarray, how: str = "mean") -> np.ndarray:
    """Aggregates columns of `features` between consecutive `bounds` (seconds)."""
    out = np.zeros((features.shape[0], max(0, len(bounds) - 1)), dtype=np.float32)
    idx = np.searchsorted(frame_times, bounds)
    for i in range(len(bounds) - 1):
        a, b = idx[i], max(idx[i] + 1, idx[i + 1])
        b = min(b, features.shape[1])
        a = min(a, b - 1) if b > 0 else 0
        seg = features[:, a:b]
        out[:, i] = np.median(seg, axis=1) if how == "median" else seg.mean(axis=1)
    return out
