"""Runs the requested tasks and builds result.json (docs/07 §4)."""

from __future__ import annotations

import platform
import traceback

import numpy as np
import scipy

from . import __version__, audio, harmony, structure, tempo

ANALYSIS_TASKS = ("tempo", "key", "chords", "structure")
TASKS = ANALYSIS_TASKS + ("stems", "midi")
DEFERRED = ()


def grid_beats(t: tempo.TempoResult, duration: float) -> np.ndarray:
    """Beat times on the fitted grid (tempo map), from beat 0 to the end of the audio."""
    pts = t.tempo_map
    out = []
    for i, p in enumerate(pts):
        end_beat = pts[i + 1]["beat"] if i + 1 < len(pts) else None
        sec_per_beat = 60.0 / p["bpm"]
        b = p["beat"]
        tt = p["timeSec"]
        while (end_beat is None or b < end_beat) and tt <= duration + 1e-6:
            out.append(tt)
            b += 1
            tt += sec_per_beat
            if len(out) > 20000:
                break
    return np.array(out)


def run(input_path: str, tasks, emit=lambda task, p: None, log=lambda msg: None, out_dir=None, stems_input=None,
        separator=None, midi_inputs=None, meter_hint=0):
    """Returns (result dict, list of error strings). Never raises for a failed task.
    Stems are written to out_dir/stems/ from stems_input (the stereo source; default input_path)."""
    result = {"input": input_path, "tasks": list(tasks), "errors": [],
              "versions": {"b4nd_analyzer": __version__, "numpy": np.__version__, "scipy": scipy.__version__,
                           "python": platform.python_version(), "backend": "builtin"}}
    errors = []
    meter_hint = meter_hint if isinstance(meter_hint, int) and 2 <= meter_hint <= 12 else 0   # beats per bar from the user
    unknown = [t for t in tasks if t not in TASKS]
    for t in unknown:
        errors.append(f"{t}: not available in this analyzer version")

    if "stems" in tasks:
        _run_stems(result, errors, stems_input or input_path, out_dir, emit, log, separator)
    if "midi" in tasks:
        _run_midi(result, errors, midi_inputs, out_dir, emit, log)
    if not any(x in tasks for x in ANALYSIS_TASKS):
        result["errors"] = errors
        return result, errors

    emit("load", 0.0)
    y = audio.load_audio(input_path)
    result["durationSec"] = round(len(y) / audio.SR, 4)
    emit("load", 1.0)
    feats = audio.compute_features(y, progress=lambda p: emit("features", p))

    t = None
    want = [x for x in ANALYSIS_TASKS if x in tasks]
    if want:
        try:
            t = tempo.analyse_tempo(feats, progress=lambda p: emit("tempo", p), meter_hint=meter_hint)
        except Exception as e:  # noqa: BLE001 - reported, other tasks still run
            errors.append(f"tempo: {e}")
            log(traceback.format_exc())
    if t is not None and "tempo" in tasks:
        result["bpm"] = {"value": t.bpm, "confidence": t.confidence,
                         "alternatives": [round(t.bpm / 2, 3), round(t.bpm * 2, 3)]}
        result["gridStartSec"] = t.grid_start
        result["tempoMap"] = t.tempo_map
        result["variableTempo"] = t.variable
        result["timeSig"] = f"{t.numerator}/4"
        result["timeSigConfidence"] = t.meter_confidence
        if meter_hint:
            result["meterHint"] = int(meter_hint)
        result["beats"] = [round(float(b), 4) for b in t.beats]

    beats = grid_beats(t, feats.duration) if t is not None else None

    if "key" in tasks:
        try:
            emit("key", 0.0)
            ct = audio.frame_time(np.arange(feats.chroma.shape[1]), hop=audio.CHROMA_HOP)
            ft = audio.frame_time(np.arange(len(feats.rms)))
            w = np.interp(ct, ft, feats.rms)
            audible = (w > w.max() * 10 ** (-40 / 20)).astype(np.float64)
            tonic, minor, conf, cands = harmony.estimate_key(feats.chroma, audible)
            result["key"] = {"value": harmony.key_name(tonic, minor), "confidence": conf, "candidates": cands}
            emit("key", 1.0)
        except Exception as e:  # noqa: BLE001
            errors.append(f"key: {e}")
            log(traceback.format_exc())

    if "chords" in tasks:
        try:
            if beats is None or len(beats) < 2:
                raise ValueError("needs the beat grid (tempo failed)")
            emit("chords", 0.0)
            bounds = np.concatenate([beats, [beats[-1] + (beats[-1] - beats[-2])]])
            bounds = bounds[bounds <= feats.duration + 1e-6]
            ft = audio.frame_time(np.arange(len(feats.rms)))
            e = audio.sync(feats.rms[None, :], ft, bounds)[0]
            e = e / (e.max() + 1e-12)
            tonic, minor = harmony.parse_key(result["key"]["value"]) if "key" in result else (0, False)
            result["chords"] = harmony.estimate_chords(feats.chroma, bounds, e, tonic, minor)
            if "key" in result:
                result["key"] = harmony.refine_key_with_chords(result["key"], result["chords"])
                t2, m2 = harmony.parse_key(result["key"]["value"])
                if (t2, m2) != (tonic, minor):   # respell with the new key
                    result["chords"] = harmony.estimate_chords(feats.chroma, bounds, e, t2, m2)
            emit("chords", 1.0)
        except Exception as e:  # noqa: BLE001
            errors.append(f"chords: {e}")
            log(traceback.format_exc())

    if "structure" in tasks:
        try:
            if t is None:
                raise ValueError("needs the beat grid (tempo failed)")
            emit("structure", 0.0)
            bars = beats[::t.numerator]
            bar_len = t.numerator * 60.0 / t.bpm
            if len(bars) and bars[-1] + bar_len <= feats.duration + bar_len * 0.5:
                bars = np.concatenate([bars, [bars[-1] + bar_len]])
            secs = structure.find_sections(feats, bars)
            result["sections"] = secs
            emit("structure", 1.0)
        except Exception as e:  # noqa: BLE001
            errors.append(f"structure: {e}")
            log(traceback.format_exc())

    if "key" in result:
        result["key"]["candidates"] = result["key"]["candidates"][:3]
    result["errors"] = errors
    return result, errors


def _run_stems(result, errors, path, out_dir, emit, log, separator):
    from . import stems

    try:
        if not out_dir:
            raise ValueError("no output folder for the stem files")
        if separator is None and not stems.demucs_available():
            raise RuntimeError("Demucs is not installed in this analyzer (pip install demucs)")
        emit("stems", 0.0)
        r = stems.run_stems(path, out_dir, separate=separator, names_versions={"model": "custom"} if separator else None,
                            progress=lambda p: emit("stems", p))
        result["stems"] = r
        result["versions"].update({k: v for k, v in r.get("versions", {}).items() if k != "device"})
        emit("stems", 1.0)
    except Exception as e:  # noqa: BLE001 - reported, the other tasks still run
        errors.append(f"stems: {e}")
        log(traceback.format_exc())


def _run_midi(result, errors, inputs, out_dir, emit, log):
    """Audio to MIDI of the given files (the song's stem tracks), or of the stems separated in
    this same run."""
    import os

    from . import transcribe

    try:
        if not inputs and "stems" in result and out_dir:
            inputs = [{"name": f["name"], "path": os.path.join(out_dir, f["path"]), "id": "stems/" + f["name"]}
                      for f in result["stems"]["files"] if not f.get("empty")]
        if not inputs:
            raise ValueError("nothing to convert (separate stems first)")
        emit("midi", 0.0)
        r, errs = transcribe.run_midi(inputs, progress=lambda p: emit("midi", p), log=log)
        result["midi"] = r
        result["versions"].update(r.get("versions", {}))
        errors.extend(errs)
        emit("midi", 1.0)
    except Exception as e:  # noqa: BLE001
        errors.append(f"midi: {e}")
        log(traceback.format_exc())
