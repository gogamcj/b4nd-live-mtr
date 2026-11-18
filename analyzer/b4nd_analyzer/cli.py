"""b4nd-analyzer command line (docs/07 §4).

    b4nd-analyzer run --job job.json     progress as JSON lines on stdout, result.json in "out"
    b4nd-analyzer info                   versions and available tasks as one JSON line
    b4nd-analyzer synth --out x.wav      synthetic test song (CI / tests)

Exit codes: 0 all tasks done, 3 some tasks failed (partial result.json written),
2 bad job or unreadable input (result.json with "errors" when "out" is known).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time

from . import __version__
from .analyze import ANALYSIS_TASKS, DEFERRED, TASKS, run


def _emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def _finite(x):
    """NaN / inf are not JSON: they become null so that the MTR can still read the rest."""
    if isinstance(x, float):
        return x if math.isfinite(x) else None
    if isinstance(x, dict):
        return {k: _finite(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [_finite(v) for v in x]
    return x


def _write_result(out_dir, result):
    result = _finite(result)
    os.makedirs(out_dir, exist_ok=True)
    tmp = os.path.join(out_dir, "result.json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, ensure_ascii=False, allow_nan=False)
    os.replace(tmp, os.path.join(out_dir, "result.json"))


def cmd_run(args) -> int:
    try:
        with open(args.job, "r", encoding="utf-8") as f:
            job = json.load(f)
    except (OSError, ValueError) as e:
        _emit({"error": f"cannot read job: {e}"})
        return 2
    base = os.path.dirname(os.path.abspath(args.job))
    out_dir = job.get("out") or base
    if not os.path.isabs(out_dir):
        out_dir = os.path.join(base, out_dir)
    inp = job.get("input", "")
    if inp and not os.path.isabs(inp):
        inp = os.path.join(base, inp)
    tasks = job.get("tasks") or list(ANALYSIS_TASKS)
    stems_input = job.get("stemsInput") or None
    if stems_input and not os.path.isabs(stems_input):
        stems_input = os.path.join(base, stems_input)
    midi_inputs = []
    for m in job.get("midiInputs") or []:
        if isinstance(m, dict) and m.get("path"):
            path = m["path"] if os.path.isabs(m["path"]) else os.path.join(base, m["path"])
            midi_inputs.append({"name": str(m.get("name", "other")), "path": path, "id": str(m.get("id", ""))})
    started = time.time()
    needs_input = any(t in tasks for t in ANALYSIS_TASKS + ("stems",))
    if needs_input and (not inp or not os.path.isfile(inp)):
        msg = f"input not found: {inp}"
        _emit({"error": msg})
        _write_result(out_dir, {"input": inp, "tasks": tasks, "errors": [msg], "status": "failed"})
        return 2

    def emit(task, p):
        _emit({"task": task, "progress": round(float(p), 3)})

    def log(msg):
        sys.stderr.write(msg + "\n")

    try:
        result, errors = run(inp, tasks, emit, log, out_dir=out_dir, stems_input=stems_input,
                             separator=_test_separator(), midi_inputs=midi_inputs)
    except Exception as e:  # noqa: BLE001 - unreadable audio etc.
        msg = f"analysis failed: {e}"
        _emit({"error": msg})
        _write_result(out_dir, {"input": inp, "tasks": tasks, "errors": [msg], "status": "failed"})
        return 2
    if isinstance(job.get("meta"), dict):
        result["job"] = job["meta"]   # echoed for the MTR (which track was analysed)
    result["status"] = "partial" if errors else "done"
    result["elapsedSec"] = round(time.time() - started, 2)
    result["device"] = result.get("stems", {}).get("versions", {}).get("device", "cpu")

    _write_result(out_dir, result)
    _emit({"done": True, "status": result["status"], "errors": errors})
    return 3 if errors else 0


def _test_separator():
    """B4ND_ANALYZER_FAKE_STEMS=1: a fast stand-in separator (tests and CI of the app without
    Demucs). Never set in a release."""
    if os.environ.get("B4ND_ANALYZER_FAKE_STEMS") != "1":
        return None
    from .stems import fake_separator

    return fake_separator


def cmd_info(_args) -> int:
    import numpy
    import scipy

    try:
        import soundfile  # noqa: F401

        sf = soundfile.__version__
    except ImportError:
        sf = None
    from .stems import demucs_info

    from .transcribe import onnx_available

    _emit({"b4nd_analyzer": __version__, "tasks": list(TASKS), "deferred": list(DEFERRED), "stems": demucs_info(),
           "midi": {"pitched": onnx_available(), "drums": True},
           "numpy": numpy.__version__, "scipy": scipy.__version__, "soundfile": sf,
           "python": sys.version.split()[0]})
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="b4nd-analyzer")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--job", required=True)
    sub.add_parser("info")
    s = sub.add_parser("synth")
    s.add_argument("--out", required=True)
    s.add_argument("--truth")
    s.add_argument("--bpm", type=float, default=128.0)
    s.add_argument("--numerator", type=int, default=4)
    s.add_argument("--lead-in", type=float, default=0.6)
    s.add_argument("--rate", type=int, default=44100)
    s.add_argument("--channels", type=int, default=2)
    s.add_argument("--tempo-change", type=float, default=0.0)
    args = p.parse_args(argv)
    if args.cmd == "run":
        return cmd_run(args)
    if args.cmd == "info":
        return cmd_info(args)
    from .synth import main_synth

    return main_synth(args)
