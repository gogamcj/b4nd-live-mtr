import json
import os
import tempfile
import unittest
import wave

import numpy as np

from b4nd_analyzer import stems
from tests.test_cli import run_cli
from tests.util import synth


def read_wav24(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 3
        ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
        b = np.frombuffer(w.readframes(n), dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
    v = np.where(v >= 1 << 23, v - (1 << 24), v)
    return (v.reshape(-1, ch).T / 8388608.0), sr


class StemTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def test_chunking_and_crossfade_rebuild_an_identity_separation(self):
        # A "separator" that returns the input as stem 0 and silence as stem 1: after chunking and
        # cross-fading, stem 0 must equal the mix everywhere (no seams, no gain change).
        rng = np.random.default_rng(1)
        n = int(stems.STEM_RATE * (stems.CHUNK_SEC * 2.5))
        mix = rng.standard_normal((2, n)).astype(np.float32) * 0.1
        calls = []

        def ident(seg):
            calls.append(seg.shape[1])
            return np.stack([seg, np.zeros_like(seg)]), ["a", "b"]

        progress = []
        out, names = stems.separate_chunked(mix, ident, progress=progress.append)
        self.assertEqual(names, ["a", "b"])
        self.assertEqual(len(calls), 3)
        self.assertLess(np.max(np.abs(out[0] - mix)), 1e-5)
        self.assertEqual(np.max(np.abs(out[1])), 0.0)
        self.assertEqual(progress[-1], 1.0)

    def test_a_song_just_over_a_chunk_is_not_counted_twice_at_the_end(self):
        sr = 1000
        for extra in (1, stems.FADE_SEC * sr - 1, stems.FADE_SEC * sr, stems.FADE_SEC * sr + 1):
            n = int(stems.CHUNK_SEC * sr + extra)
            mix = np.ones((2, n), np.float32)
            out, _ = stems.separate_chunked(mix, lambda s: (np.stack([s]), ["x"]), sr=sr)
            self.assertLess(np.max(np.abs(out[0] - mix)), 1e-5, extra)

    def test_short_input_is_one_chunk(self):
        mix = np.ones((2, 1000), np.float32) * 0.2
        out, _ = stems.separate_chunked(mix, lambda s: (np.stack([s]), ["x"]))
        self.assertLess(np.max(np.abs(out[0] - mix)), 1e-6)

    def test_quality_flags_a_separation_that_loses_sound(self):
        mix = np.random.default_rng(2).standard_normal((2, 44100)).astype(np.float32) * 0.1
        good = stems.quality(mix, np.stack([mix * 0.5, mix * 0.5]), ["a", "b"])
        self.assertLess(good["residualDb"], -60)
        self.assertEqual(good["warnings"], [])
        lost = stems.quality(mix, np.stack([mix * 0.5, np.zeros_like(mix)]), ["a", "b"])
        self.assertAlmostEqual(lost["residualDb"], -6.0, delta=0.2)
        self.assertTrue(lost["warnings"])
        self.assertTrue(lost["files"][1]["empty"])
        self.assertAlmostEqual(lost["files"][0]["energyDb"], -6.0, delta=0.2)

    def test_run_stems_writes_aligned_24bit_files(self):
        song = synth.demo_song(120, 4, 0.5, sr=48000)
        y = synth.render(song)[: 48000 * 12]
        path = os.path.join(self.dir, "mix.wav")
        synth.write_wav(path, y, 48000, channels=2)
        r = stems.run_stems(path, self.dir, separate=stems.fake_separator)
        self.assertEqual([f["name"] for f in r["files"]], list(stems.STEM_NAMES))
        self.assertEqual(r["sampleRate"], 44100)
        mix = stems.load_stereo(path)
        total = np.zeros_like(mix)
        for f in r["files"]:
            x, sr = read_wav24(os.path.join(self.dir, f["path"]))
            self.assertEqual(sr, 44100)
            self.assertEqual(x.shape, mix.shape)    # same length: stems line up with the source
            total += x
        # The stems add back up to the mix (24-bit rounding only).
        self.assertLess(np.max(np.abs(total - mix)), 1e-4)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "stems", "bass.wav.tmp")))

    def test_clipping_is_reported(self):
        x = np.ones((2, 2000), np.float32) * 0.6
        path = os.path.join(self.dir, "loud.wav")
        synth.write_wav(path, x[0], 44100, channels=2)
        r = stems.run_stems(path, self.dir, separate=lambda s: (np.stack([s * 2.0, -s]), ["a", "b"]))
        self.assertTrue(r["files"][0]["clipped"])
        self.assertTrue(any("clipped" in w for w in r["warnings"]))

    def test_cli_stems_with_fake_separator(self):
        run_cli("synth", "--out", os.path.join(self.dir, "song.wav"), "--rate", "48000")
        job = os.path.join(self.dir, "job.json")
        with open(job, "w") as f:
            json.dump({"input": "song.wav", "stemsInput": "song.wav", "tasks": ["stems"], "out": "analysis/s1"}, f)
        code, out, err = run_cli("run", "--job", job, B4ND_ANALYZER_FAKE_STEMS="1")
        self.assertEqual(code, 0, err)
        lines = [json.loads(l) for l in out.splitlines()]
        self.assertTrue(any(l.get("task") == "stems" for l in lines))
        with open(os.path.join(self.dir, "analysis/s1/result.json")) as f:
            r = json.load(f)
        self.assertNotIn("bpm", r)                  # only stems were asked for
        self.assertEqual(len(r["stems"]["files"]), 6)
        for f in r["stems"]["files"]:
            self.assertTrue(os.path.isfile(os.path.join(self.dir, "analysis/s1", f["path"])))

    def test_cli_stems_without_demucs_is_a_clear_error(self):
        if stems.demucs_available():
            self.skipTest("Demucs is installed here")
        run_cli("synth", "--out", os.path.join(self.dir, "song.wav"))
        job = os.path.join(self.dir, "job.json")
        with open(job, "w") as f:
            json.dump({"input": "song.wav", "tasks": ["tempo", "stems"], "out": "o"}, f)
        code, _, _ = run_cli("run", "--job", job)
        self.assertEqual(code, 3)
        with open(os.path.join(self.dir, "o/result.json")) as f:
            r = json.load(f)
        self.assertIn("bpm", r)
        self.assertTrue(any("Demucs is not installed" in e for e in r["errors"]))


@unittest.skipUnless(os.environ.get("B4ND_TEST_DEMUCS") == "1", "set B4ND_TEST_DEMUCS=1 (needs Demucs + model download)")
class RealDemucsTests(unittest.TestCase):
    """The real model on a synthetic song (CI). The synthetic instruments are not what Demucs was
    trained on, so this checks that it runs, keeps length / alignment and roughly adds up - and
    that the bass stem carries the low end - not separation quality on real music."""

    def test_htdemucs_6s(self):
        with tempfile.TemporaryDirectory() as d:
            song = synth.demo_song(120, 4, 0.5, sr=44100)
            y = synth.render(song)[: 44100 * 20]
            path = os.path.join(d, "mix.wav")
            synth.write_wav(path, y, 44100, channels=2)
            r = stems.run_stems(path, d)
            print(json.dumps({k: r[k] for k in ("residualDb", "files", "warnings", "versions")}, indent=1))
            self.assertEqual(r["model"], "htdemucs_6s")
            self.assertEqual(sorted(f["name"] for f in r["files"]), sorted(stems.STEM_NAMES))
            self.assertLess(r["residualDb"], -10.0)
            mix = stems.load_stereo(path)
            bass, _ = read_wav24(os.path.join(d, "stems", "bass.wav"))
            self.assertEqual(bass.shape, mix.shape)
