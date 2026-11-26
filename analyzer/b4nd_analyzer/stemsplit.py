"""Finer stems after the separation (task "stemsplit"): guitar by stereo position, synth pads
from the rest of "other". Own DSP on numpy / scipy, no model download, runs on every platform.

Why not a model: no open model splits lead / rhythm guitar or synth pads (checked 2026-11:
open 6-stem models such as htdemucs_6s and BS-RoFormer SW stop at one guitar stem; MVSep's
guitar models are a web service and also give one guitar). What most band recordings do have:
- the two guitars (1st / 2nd, or a double-tracked rhythm) are panned left and right, and a lead
  or solo often sits in the middle. So the guitar stem is split into left / center / right by
  where each time-frequency point sits between the channels.
- pads are long held chords. So "other" is split into what is sustained (pad) and the rest
  (arps, plucks, hits, effects) by harmonic / percussive median filtering with a long window.

Each split is a set of soft masks that add up to 1 at every point, so the parts add back up to
the stem they came from (checked by a test). The parent stem is always kept as well.

Known limits (said in docs/07 and in the result warnings):
- a stereo position is not a role: when both guitars are in the middle, or one guitar is spread
  over both sides with a stereo effect, there is nothing to split ("mostly center").
- the pad split also takes held organ / strings / piano chords, and a pad with a fast filter
  or tremolo leaks into the rest.
"""

from __future__ import annotations

import numpy as np
from scipy import ndimage, signal

from .stems import STEM_RATE

N_FFT = 4096
HOP = 1024
PART_EMPTY_DB = -24.0     # a part this much quieter than its stem is not worth a track
CHUNK_SEC = 30.0          # bounded memory on long songs (stems.separate_chunked cross-fades)

# Pan index p in -1 (only left) .. 1 (only right). Full side below / above +-PAN_FULL, a soft
# ramp from +-PAN_START so that guitars panned to 50 % still land on their side.
PAN_START = 0.12
PAN_FULL = 0.45
COH_BINS, COH_FRAMES = 1, 9   # neighbourhood for the channel coherence (one bin x ~210 ms)
COH_LOW, COH_HIGH = 0.6, 0.95 # below: the channels hold different sounds; above: one sound

PAD_TIME_SEC = 0.8        # a pad holds a note longer than this
PAD_FREQ_HZ = 250.0       # a hit / pluck spreads over more than this


def _stft(x):
    if x.shape[-1] < N_FFT:   # the last chunk of a song can be a few samples: pad (cut off again in _istft)
        x = np.pad(x, ((0, 0), (0, N_FFT - x.shape[-1])))
    _, _, z = signal.stft(x, nperseg=N_FFT, noverlap=N_FFT - HOP, window="hann", boundary="even", padded=True)
    return z


def _istft(z, n):
    _, y = signal.istft(z, nperseg=N_FFT, noverlap=N_FFT - HOP, window="hann", boundary=True)
    out = np.zeros((z.shape[0], n), dtype=np.float32)
    m = min(n, y.shape[-1])
    out[:, :m] = y[:, :m]
    return out


def _apply(z, masks, n):
    """Masked complex spectra -> parts. A mask is (bins, frames) for both channels or
    (2, bins, frames) per channel. The masks add up to 1, so the parts add up to the input (up
    to the STFT round trip)."""
    return [_istft(z * (m if m.ndim == 3 else m[None]), n) for m in masks]


def _smooth(x):
    """Local average over COH_BINS x COH_FRAMES (complex values: real and imaginary apart)."""
    size = (COH_BINS, COH_FRAMES)
    if np.iscomplexobj(x):
        return ndimage.uniform_filter(x.real, size=size) + 1j * ndimage.uniform_filter(x.imag, size=size)
    return ndimage.uniform_filter(x, size=size)


def pan_masks(zl, zr):
    """Per channel masks (left, center, right), each (2, bins, frames), that add up to 1 on each
    channel.

    Each time-frequency point is looked at in a small neighbourhood:
    - coherence c (0..1) of the channels: one source panned somewhere gives the same signal on
      both channels (c near 1); two different guitars on the two sides do not (c near 0).
    - the coherent share goes to left / center / right by the level difference (pan index).
    - the incoherent share keeps its channel: left channel -> left part, right -> right. That
      is what keeps two hard-panned guitars apart where their notes overlap."""
    pl, pr = np.abs(zl) ** 2, np.abs(zr) ** 2
    sll, srr = np.maximum(_smooth(pl), 0.0), np.maximum(_smooth(pr), 0.0)   # filters can round below 0
    slr = np.abs(_smooth(zl * np.conj(zr)))
    coh = np.nan_to_num(slr / np.sqrt(sll * srr + 1e-20))
    c = np.clip((coh - COH_LOW) / (COH_HIGH - COH_LOW), 0.0, 1.0)
    al, ar = np.sqrt(sll), np.sqrt(srr)
    p = (ar - al) / (al + ar + 1e-9)
    ramp = lambda v: np.clip((v - PAN_START) / (PAN_FULL - PAN_START), 0.0, 1.0)   # noqa: E731
    wl, wr = ramp(-p), ramp(p)
    wc = 1.0 - wl - wr
    left = np.stack([c * wl + (1.0 - c), c * wl])
    right = np.stack([c * wr, c * wr + (1.0 - c)])
    center = np.stack([c * wc, c * wc])
    return left.astype(np.float32), center.astype(np.float32), right.astype(np.float32)


GUITAR_PARTS = ("guitar_left", "guitar_center", "guitar_right")


def split_guitar(seg: np.ndarray):
    """2 x n guitar stem -> (3, 2, n) left / center / right parts."""
    n = seg.shape[1]
    z = _stft(seg)
    parts = _apply(z, pan_masks(z[0], z[1]), n)
    return np.stack(parts).astype(np.float32), list(GUITAR_PARTS)


PAD_PARTS = ("synth_pad", "other_rest")


def sustain_mask(z):
    """Share of each time-frequency point that is sustained: median over a long time window
    (held notes survive) against a median over frequency (hits and plucks survive)."""
    power = np.sum(np.abs(z) ** 2, axis=0)
    frames = max(3, int(round(PAD_TIME_SEC * STEM_RATE / HOP)) | 1)
    bins = max(3, int(round(PAD_FREQ_HZ * N_FFT / STEM_RATE)) | 1)
    mag = np.sqrt(power)
    h = ndimage.median_filter(mag, size=(1, frames), mode="nearest")
    v = ndimage.median_filter(mag, size=(bins, 1), mode="nearest")
    h2, v2 = h * h, v * v
    return (h2 / (h2 + v2 + 1e-12)).astype(np.float32)


def split_pad(seg: np.ndarray):
    """2 x n "other" stem -> (2, 2, n) sustained (pad) / rest parts."""
    n = seg.shape[1]
    z = _stft(seg)
    m = sustain_mask(z)
    parts = _apply(z, (m, 1.0 - m), n)
    return np.stack(parts).astype(np.float32), list(PAD_PARTS)


SPLITS = {"guitar": split_guitar, "other": split_pad}


def _rms(x):
    return float(np.sqrt(np.mean(np.square(x)))) if x.size else 0.0


def _db(x):
    return float(20.0 * np.log10(max(x, 1e-12)))


def split_stem(name: str, stem: np.ndarray, progress=lambda p: None):
    """Returns (parts, names, info) for one parent stem, or None when it has no split.
    info: energy of each part against the parent, and a note when the split found nothing."""
    from .stems import separate_chunked

    fn = SPLITS.get(name)
    if fn is None:
        return None
    parts, names = separate_chunked(stem, fn, progress=progress, chunk_sec=CHUNK_SEC)
    parts = np.nan_to_num(parts)
    parent = _rms(stem) + 1e-12
    rel = [_db(_rms(p) / parent) for p in parts]
    notes = []
    if name == "guitar":
        side = max(rel[0], rel[2])
        if side < PART_EMPTY_DB:
            notes.append("guitar: almost everything is in the center (no left / right guitars to split)")
        elif min(rel[0], rel[2]) < PART_EMPTY_DB:
            notes.append("guitar: only one side has a guitar")
    if name == "other" and rel[0] < PART_EMPTY_DB:
        notes.append("other: no sustained pad found")
    return parts, names, {"relDb": rel, "notes": notes}
