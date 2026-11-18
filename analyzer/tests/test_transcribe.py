"""Audio to MIDI on synthetic stems with known notes (Basic Pitch via onnxruntime, own drums)."""

import json
import os
import tempfile
import unittest

import numpy as np

from b4nd_analyzer import transcribe as T
from tests.test_cli import run_cli
from tests.util import synth

SR = 44100
NEED_ONNX = os.environ.get("B4ND_REQUIRE_ONNX") == "1"


def tone(pitch, dur, harmonics, decay):
    t = np.arange(int(dur * SR)) / SR
    f = 440.0 * 2 ** ((pitch - 69) / 12.0)
    env = np.minimum(1, t / 0.004) * np.exp(-t / decay) * np.minimum(1, (dur - t) / 0.015)
    return sum(a * np.sin(2 * np.pi * f * (i + 1) * t) for i, a in enumerate(harmonics)) * env


def place(y, at, sig, gain):
    i = int(round(at * SR))
    m = min(len(sig), len(y) - i)
    y[i:i + m] += gain * sig[:m]


def bass_line(seconds=20.0, seed=3):
    rng = np.random.default_rng(seed)
    y, truth, t = np.zeros(int(SR * seconds)), [], 0.5
    while t < seconds - 1.0:
        p, d = int(rng.integers(28, 52)), float(rng.choice([0.25, 0.5, 0.75]))
        place(y, t, tone(p, d * 0.95, [1, 0.5, 0.3, 0.2], 0.6), 0.3)
        truth.append((t, p))
        t += d
    return y, truth


def piano_part():
    chords = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]
    y, truth, t = np.zeros(SR * 18), [], 0.3
    for rep in range(4):
        for ch in chords:
            for p in ch:
                place(y, t, tone(p, 0.95, [1, 0.5, 0.3, 0.2, 0.1], 0.8), 0.2)
                truth.append((t, p))
            p = 72 + [0, 2, 4, 5][rep]
            place(y, t + 0.5, tone(p, 0.45, [1, 0.5, 0.3, 0.2, 0.1], 0.8), 0.2)
            truth.append((t + 0.5, p))
            t += 1.0
    return y, truth


def drum_part(bpm=110.0, seconds=12.0):
    rng = np.random.default_rng(1)

    def env(n, a, d):
        t = np.arange(n) / SR
        return np.minimum(1, t / a) * np.exp(-t / d)

    kn = int(0.18 * SR)
    tk = np.arange(kn) / SR
    kick = np.sin(2 * np.pi * (50 * tk + 60 * (1 - np.exp(-tk * 30)) / 30)) * env(kn, 0.001, 0.07)
    sn = int(0.15 * SR)
    snare = rng.standard_normal(sn) * env(sn, 0.001, 0.04) * 0.5 + np.sin(2 * np.pi * 190 * np.arange(sn) / SR) * env(sn, 0.001, 0.03) * 0.4
    hn = int(0.04 * SR)
    hat = np.diff(rng.standard_normal(hn + 1)) * env(hn, 0.0005, 0.01) * 0.3
    y, truth, beat, t, k = np.zeros(int(SR * seconds)), [], 60.0 / bpm, 0.4, 0
    while t < seconds - 0.5:
        if k % 4 in (0, 2):
            place(y, t, kick, 0.9); truth.append((t, 36))
        else:
            place(y, t, snare, 0.8); truth.append((t, 38))
        place(y, t, hat, 0.5); truth.append((t, 42))
        place(y, t + beat / 2, hat, 0.35); truth.append((t + beat / 2, 42))
        t += beat
        k += 1
    return y, truth


def score(notes, truth, tol=0.05):
    hits = [(a, p) for a, p in truth if any(abs(n[0] - a) < tol and n[2] == p for n in notes)]
    false = [n for n in notes if not any(abs(n[0] - a) < tol and n[2] == p for a, p in truth)]
    offs = [min((n[0] - a for n in notes if n[2] == p), key=abs) for a, p in hits]
    return len(hits) / len(truth), 1.0 - len(false) / max(1, len(notes)), offs


class TranscribeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def wav(self, name, y):
        path = os.path.join(self.tmp.name, name + ".wav")
        synth.write_wav(path, (y / np.abs(y).max() * 0.7).astype(np.float32), SR)
        return path

    def model(self):
        if not T.onnx_available():
            if NEED_ONNX:
                self.fail("onnxruntime required (B4ND_REQUIRE_ONNX=1)")
            self.skipTest("onnxruntime not installed")
        return T.BasicPitch()

    def test_frame_times_follow_the_windowing(self):
        # Frames step by one hop inside a window; across windows, by the window step.
        t = T.frame_time(np.arange(T.BP_KEEP * 3))
        self.assertAlmostEqual(float(t[T.BP_KEEP] - t[0]), T.BP_STEP / T.BP_SR, places=6)
        self.assertAlmostEqual(float(t[1] - t[0]), T.BP_HOP / T.BP_SR, places=6)

    def test_bass_line_is_one_note_at_a_time_with_the_right_pitches(self):
        m = self.model()
        y, truth = bass_line()
        r = T.transcribe_file(self.wav("bass", y), "bass", m)
        recall, precision, offs = score(r["notes"], truth)
        print(f"bass: recall {recall:.2f} precision {precision:.2f} onset median {1000 * np.median(offs):.1f} ms")
        self.assertGreaterEqual(recall, 0.85)
        self.assertGreaterEqual(precision, 0.7)
        self.assertLess(abs(np.median(offs)), 0.012)
        for a, b in zip(r["notes"], r["notes"][1:]):
            self.assertLessEqual(a[1], b[0] + 1e-6)   # monophonic: no overlaps
        self.assertEqual(r["program"], 33)
        self.assertFalse(r["drums"])

    def test_piano_chords_and_melody(self):
        m = self.model()
        y, truth = piano_part()
        r = T.transcribe_file(self.wav("piano", y), "piano", m)
        recall, precision, offs = score(r["notes"], truth)
        print(f"piano: recall {recall:.2f} precision {precision:.2f} ({len(r['notes'])} notes for {len(truth)})")
        self.assertGreaterEqual(recall, 0.9)
        self.assertGreaterEqual(precision, 0.55)   # synthetic tones: strong harmonics come out as extra notes
        self.assertTrue(all(1 <= n[3] <= 127 and 0.0 <= n[4] <= 1.0 for n in r["notes"]))

    def test_drums_become_kick_snare_hihat(self):
        y, truth = drum_part()
        r = T.transcribe_file(self.wav("drums", y), "drums")
        self.assertTrue(r["drums"])
        for pitch in (36, 38, 42):
            tr = [(a, p) for a, p in truth if p == pitch]
            est = [n for n in r["notes"] if n[2] == pitch]
            recall, precision, offs = score(est, tr, tol=0.03)
            print(f"drum {pitch}: recall {recall:.2f} precision {precision:.2f} offset {1000 * np.median(offs):.1f} ms")
            self.assertGreaterEqual(recall, 0.9)
            self.assertGreaterEqual(precision, 0.9)
            self.assertLess(abs(np.median(offs)), 0.008)

    def test_silence_gives_no_notes(self):
        y = np.zeros(SR * 5)
        y[0] = 1e-4
        self.assertEqual(T.transcribe_file(self.wav("quiet", y), "drums")["notes"], [])
        if T.onnx_available():
            self.assertEqual(T.transcribe_file(self.wav("quiet2", y), "piano", T.BasicPitch())["notes"], [])

    def test_cli_midi_from_given_stems(self):
        self.model()
        yb, _ = bass_line(8.0)
        yd, _ = drum_part(seconds=8.0)
        job = os.path.join(self.tmp.name, "job.json")
        with open(job, "w") as f:
            json.dump({"tasks": ["midi"], "out": "out",
                       "midiInputs": [{"name": "bass", "path": self.wav("b", yb), "id": "track-1"},
                                      {"name": "drums", "path": self.wav("d", yd), "id": "track-2"},
                                      {"name": "piano", "path": "missing.wav", "id": "track-3"}]}, f)
        code, out, err = run_cli("run", "--job", job)
        self.assertEqual(code, 3, err)   # the missing file fails alone
        with open(os.path.join(self.tmp.name, "out", "result.json")) as f:
            r = json.load(f)
        names = [t["name"] for t in r["midi"]["tracks"]]
        self.assertEqual(names, ["bass", "drums"])
        self.assertEqual(r["midi"]["tracks"][0]["sourceId"], "track-1")
        self.assertTrue(any(e.startswith("midi piano:") for e in r["errors"]))
        self.assertTrue(all(len(t["notes"]) > 5 for t in r["midi"]["tracks"]))
        self.assertTrue(any(json.loads(l).get("task") == "midi" for l in out.splitlines()))
