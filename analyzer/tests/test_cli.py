import json
import os
import subprocess
import sys
import tempfile
import unittest
import wave

import numpy as np

from tests.util import synth

ROOT = os.path.join(os.path.dirname(__file__), "..")


def run_cli(*args, **extra_env):
    env = dict(os.environ, PYTHONPATH=os.path.abspath(ROOT), **extra_env)
    p = subprocess.run([sys.executable, "-m", "b4nd_analyzer", *args], capture_output=True, text=True, env=env,
                       timeout=300)
    return p.returncode, p.stdout, p.stderr


class CliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def job(self, inp, tasks, out="analysis/song-1"):
        path = os.path.join(self.dir, "job.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"input": inp, "tasks": tasks, "device": "cpu", "out": out}, f)
        return path

    def result(self, out="analysis/song-1"):
        with open(os.path.join(self.dir, out, "result.json"), encoding="utf-8") as f:
            return json.load(f)

    def test_run_writes_result_and_progress_lines(self):
        code, _, _ = run_cli("synth", "--out", os.path.join(self.dir, "song.wav"), "--bpm", "120", "--rate", "48000")
        self.assertEqual(code, 0)
        code, out, err = run_cli("run", "--job", self.job("song.wav", ["tempo", "key", "chords", "structure"]))
        self.assertEqual(code, 0, err)
        lines = [json.loads(l) for l in out.splitlines()]   # every stdout line is JSON
        self.assertTrue(any(l.get("task") == "tempo" for l in lines))
        self.assertTrue(all(0.0 <= l["progress"] <= 1.0 for l in lines if "progress" in l))
        self.assertEqual(lines[-1], {"done": True, "status": "done", "errors": []})
        r = self.result()
        for k in ("bpm", "tempoMap", "timeSig", "key", "chords", "sections", "versions", "gridStartSec"):
            self.assertIn(k, r)
        self.assertAlmostEqual(r["bpm"]["value"], 120.0, delta=0.05)
        self.assertEqual(r["status"], "done")

    def test_unavailable_task_gives_partial_result(self):
        run_cli("synth", "--out", os.path.join(self.dir, "song.wav"))
        code, out, _ = run_cli("run", "--job", self.job("song.wav", ["tempo", "lyrics"]))
        self.assertEqual(code, 3)
        r = self.result()
        self.assertEqual(r["status"], "partial")
        self.assertIn("bpm", r)                      # the tasks that worked are kept
        self.assertTrue(any(e.startswith("lyrics:") for e in r["errors"]))

    def test_missing_input_fails_with_a_result_file(self):
        code, out, _ = run_cli("run", "--job", self.job("nope.wav", ["tempo"]))
        self.assertEqual(code, 2)
        r = self.result()
        self.assertEqual(r["status"], "failed")
        self.assertIn("input not found", r["errors"][0])

    def test_silence_reports_errors_instead_of_guessing(self):
        path = os.path.join(self.dir, "silence.wav")
        synth.write_wav(path, np.zeros(44100 * 8, np.float32), 44100)
        code, _, _ = run_cli("run", "--job", self.job("silence.wav", ["tempo", "key", "chords", "structure"]))
        self.assertEqual(code, 3)
        r = self.result()
        self.assertNotIn("bpm", r)
        self.assertNotIn("chords", r)
        self.assertTrue(any(e.startswith("tempo:") for e in r["errors"]))

    def test_corrupt_file_fails_cleanly(self):
        path = os.path.join(self.dir, "bad.wav")
        with open(path, "wb") as f:
            f.write(b"RIFF\x00\x00\x00\x00WAVEjunkjunk")
        code, out, _ = run_cli("run", "--job", self.job("bad.wav", ["tempo"]))
        self.assertEqual(code, 2)
        self.assertEqual(self.result()["status"], "failed")

    def test_reads_24_bit_wav(self):
        song = synth.demo_song(126.0, sr=44100)
        y = synth.render(song)
        v = np.clip(np.round(y * 8388607.0), -8388608, 8388607).astype(np.int32)
        b = np.stack([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF], axis=1).astype(np.uint8)
        path = os.path.join(self.dir, "s24.wav")
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(3)
            w.setframerate(44100)
            w.writeframes(b.tobytes())
        for env in ({}, {"B4ND_ANALYZER_NO_SOUNDFILE": "1"}):   # with and without soundfile
            with self.subTest(env=env):
                code, _, err = run_cli("run", "--job", self.job("s24.wav", ["tempo"]), **env)
                self.assertEqual(code, 0, err)
                self.assertAlmostEqual(self.result()["bpm"]["value"], 126.0, delta=0.05)

    def test_info(self):
        code, out, _ = run_cli("info")
        self.assertEqual(code, 0)
        info = json.loads(out)
        self.assertEqual(info["tasks"], ["tempo", "key", "chords", "structure", "stems", "midi"])
        self.assertTrue(info["midi"]["drums"])
        self.assertIn("available", info["stems"])


if __name__ == "__main__":
    unittest.main()


class ResultFileTests(unittest.TestCase):
    def test_non_finite_numbers_become_null(self):
        from b4nd_analyzer import cli

        with tempfile.TemporaryDirectory() as d:
            cli._write_result(d, {"bpm": {"value": float("nan"), "confidence": float("inf")}, "x": [1.0, float("-inf")]})
            with open(os.path.join(d, "result.json"), encoding="utf-8") as f:
                text = f.read()
            self.assertNotIn("NaN", text)
            self.assertNotIn("Infinity", text)
            r = json.loads(text)
            self.assertIsNone(r["bpm"]["value"])
            self.assertEqual(r["x"], [1.0, None])
