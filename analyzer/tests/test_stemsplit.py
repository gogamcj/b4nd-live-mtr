"""Finer stems (stemsplit.py): guitar left / center / right, synth pad / rest."""

import json
import os
import tempfile
import unittest

import numpy as np

from b4nd_analyzer import stems, stemsplit, transcribe
from tests.test_cli import run_cli

SR = stems.STEM_RATE


def plucks(freqs, dur, step, decay, seed=0):
    """A guitar-like line: harmonic plucks every `step` seconds, cycling through `freqs`."""
    n = int(dur * SR)
    y = np.zeros(n)
    for k, t0 in enumerate(np.arange(0.0, dur, step)):
        f = freqs[k % len(freqs)]
        i0 = int(t0 * SR)
        m = min(n - i0, int(step * SR * 2))
        t = np.arange(m) / SR
        env = np.exp(-decay * t) * (1.0 - np.exp(-t * 400.0))
        y[i0:i0 + m] += env * sum(np.sin(2 * np.pi * f * h * t) / h for h in range(1, 12) if f * h < 15000)
    return y / np.max(np.abs(y)) * 0.3


def pan(y, p):
    """Constant power pan, p -1 (left) .. 1 (right)."""
    a = (p + 1.0) * np.pi / 4.0
    return np.stack([y * np.cos(a), y * np.sin(a)])


def sdr(ref, est):
    return 10.0 * np.log10(np.sum(ref ** 2) / np.sum((ref - est) ** 2))


def guitars(dur=12.0, side=1.0):
    g1 = plucks([82.4, 123.5, 164.8, 207.7], dur, 0.125, 6.0)     # 1st guitar
    g2 = plucks([138.6, 185.0, 233.1, 277.2], dur, 0.25, 5.0)     # 2nd guitar
    lead = plucks([466.2, 523.3, 622.3, 698.5, 784.0], dur, 0.5, 2.0)
    return [pan(g1, -side), pan(lead, 0.0), pan(g2, side)]


def pad_and_rest(dur=12.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(3)
    pad = sum(np.sin(2 * np.pi * f * t + rng.uniform(0, 6)) * 0.1 for f in (220.0, 277.2, 329.6, 440.0, 554.4))
    pad = pad * np.minimum(1.0, t / 1.0)
    arp = plucks([659.3, 880.0, 987.8, 1318.5], dur, 0.125, 25.0)
    hits = np.zeros(n)
    for i in range(0, n - 2000, SR // 2):
        hits[i:i + 2000] += rng.standard_normal(2000) * np.exp(-np.arange(2000) / 300.0) * 0.3
    return np.stack([pad, pad * 0.95]), np.stack([arp + hits, arp + hits])


class GuitarSplitTests(unittest.TestCase):
    def test_hard_panned_guitars_and_a_center_lead(self):
        truth = guitars()
        mix = sum(truth).astype(np.float32)
        parts, names, info = stemsplit.split_stem("guitar", mix)
        self.assertEqual(names, list(stemsplit.GUITAR_PARTS))
        scores = [sdr(a, b) for a, b in zip(truth, parts)]
        self.assertGreater(scores[0], 12.0, scores)   # left guitar
        self.assertGreater(scores[1], 8.0, scores)    # center lead
        self.assertGreater(scores[2], 12.0, scores)   # right guitar
        self.assertEqual(info["notes"], [])
        # The parts add back up to the guitar stem (the stem itself is kept too).
        self.assertLess(np.max(np.abs(parts.sum(axis=0) - mix)), 1e-4)

    def test_half_panned_guitars_still_land_on_their_side(self):
        truth = guitars(side=0.6)
        parts, _, _ = stemsplit.split_stem("guitar", sum(truth).astype(np.float32))
        self.assertGreater(sdr(truth[0], parts[0]), 8.0)
        self.assertGreater(sdr(truth[2], parts[2]), 8.0)

    def test_a_mono_guitar_has_nothing_to_split(self):
        g = plucks([110.0, 146.8, 196.0], 8.0, 0.25, 4.0)
        mix = np.stack([g, g]).astype(np.float32)
        parts, _, info = stemsplit.split_stem("guitar", mix)
        self.assertLess(info["relDb"][0], stemsplit.PART_EMPTY_DB)
        self.assertLess(info["relDb"][2], stemsplit.PART_EMPTY_DB)
        self.assertTrue(any("center" in n for n in info["notes"]))

    def test_long_stems_are_split_in_chunks_without_seams(self):
        truth = guitars(dur=stemsplit.CHUNK_SEC * 2.2)
        mix = sum(truth).astype(np.float32)
        parts, _, _ = stemsplit.split_stem("guitar", mix)
        self.assertLess(np.max(np.abs(parts.sum(axis=0) - mix)), 1e-4)
        self.assertGreater(sdr(truth[0], parts[0]), 12.0)


    def test_a_last_chunk_of_a_few_samples(self):
        truth = guitars(dur=stemsplit.CHUNK_SEC + 0.01)
        mix = sum(truth).astype(np.float32)[:, : int(stemsplit.CHUNK_SEC * SR) + 100]
        parts, _, _ = stemsplit.split_stem("guitar", mix)
        self.assertEqual(parts.shape, (3,) + mix.shape)
        self.assertLess(np.max(np.abs(parts.sum(axis=0) - mix)), 1e-4)
        tiny = mix[:, :500]
        self.assertEqual(stemsplit.split_guitar(tiny)[0].shape, (3, 2, 500))
        self.assertEqual(stemsplit.split_pad(tiny)[0].shape, (2, 2, 500))


class PadSplitTests(unittest.TestCase):
    def test_sustained_pad_is_taken_out_of_other(self):
        pad, rest = pad_and_rest()
        mix = (pad + rest).astype(np.float32)
        parts, names, info = stemsplit.split_stem("other", mix)
        self.assertEqual(names, list(stemsplit.PAD_PARTS))
        self.assertGreater(sdr(pad, parts[0]), 9.0)
        self.assertGreater(sdr(rest, parts[1]), 4.0)
        self.assertLess(np.max(np.abs(parts.sum(axis=0) - mix)), 1e-4)

    def test_no_pad_is_reported(self):
        _, rest = pad_and_rest()
        _, _, info = stemsplit.split_stem("other", rest.astype(np.float32))
        self.assertLess(info["relDb"][0], info["relDb"][1])

    def test_other_stems_have_no_split(self):
        self.assertIsNone(stemsplit.split_stem("bass", np.zeros((2, 1000), np.float32)))


class RunStemsSplitTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def test_parts_are_extra_files_next_to_the_stems(self):
        from tests.test_stems import read_wav24

        g = sum(guitars(dur=10.0))
        pad, rest = pad_and_rest(dur=10.0)
        mix = (g + pad + rest).astype(np.float32)
        path = os.path.join(self.dir, "mix.wav")
        stems.write_wav24(path, mix, SR)
        # A "separator" that knows the answer: guitar, other (pad + rest), and an empty piano.
        src = {"guitar": g, "other": pad + rest}

        def sep(seg, pos=[0]):
            n = seg.shape[1]
            out = np.stack([src["guitar"][:, pos[0]:pos[0] + n], src["other"][:, pos[0]:pos[0] + n],
                            np.zeros((2, n))]).astype(np.float32)
            return out, ["guitar", "other", "piano"]

        r = stems.run_stems(path, self.dir, separate=sep, split=True)
        names = [f["name"] for f in r["files"]]
        self.assertEqual(names[:3], ["guitar", "other", "piano"])
        self.assertEqual(sorted(names[3:]), sorted(stemsplit.GUITAR_PARTS + stemsplit.PAD_PARTS))
        self.assertLess(r["residualDb"], -60)    # of the parent stems only, parts not counted twice
        for f in r["files"][3:]:
            self.assertIn(f["parent"], ("guitar", "other"))
            self.assertEqual(f["path"], f"stems/{f['name']}.wav")
            self.assertFalse(f["empty"], f)
            x, sr = read_wav24(os.path.join(self.dir, f["path"]))
            self.assertEqual((sr, x.shape), (SR, mix.shape))

    def test_without_split_nothing_changes(self):
        x = np.random.default_rng(1).standard_normal((2, SR * 2)).astype(np.float32) * 0.1
        path = os.path.join(self.dir, "mix.wav")
        stems.write_wav24(path, x, SR)
        r = stems.run_stems(path, self.dir, separate=lambda s: (np.stack([s]), ["guitar"]))
        self.assertEqual([f["name"] for f in r["files"]], ["guitar"])

    def test_cli_stemsplit_with_fake_separator(self):
        run_cli("synth", "--out", os.path.join(self.dir, "song.wav"))
        job = os.path.join(self.dir, "job.json")
        with open(job, "w") as f:
            json.dump({"input": "song.wav", "stemsInput": "song.wav", "tasks": ["stems", "stemsplit"], "out": "o"}, f)
        code, _, err = run_cli("run", "--job", job, B4ND_ANALYZER_FAKE_STEMS="1")
        self.assertEqual(code, 0, err)
        with open(os.path.join(self.dir, "o/result.json")) as f:
            r = json.load(f)
        names = [f["name"] for f in r["stems"]["files"]]
        self.assertEqual(names[:6], list(stems.STEM_NAMES))
        self.assertTrue(set(stemsplit.GUITAR_PARTS) <= set(names))
        # The synthetic song is mono: its guitar is all in the center, and that is said.
        parts = {f["name"]: f for f in r["stems"]["files"]}
        # No part is a copy of its stem: a split that found nothing adds no track.
        self.assertTrue(all(parts[n]["empty"] for n in stemsplit.GUITAR_PARTS))
        self.assertTrue(all(parts[n]["empty"] for n in stemsplit.PAD_PARTS))
        for f in r["stems"]["files"]:
            self.assertIsNotNone(f["energyDb"])
        self.assertTrue(any("center" in w for w in r["stems"]["warnings"]))

    def test_cli_stemsplit_needs_stems(self):
        run_cli("synth", "--out", os.path.join(self.dir, "song.wav"))
        job = os.path.join(self.dir, "job.json")
        with open(job, "w") as f:
            json.dump({"input": "song.wav", "tasks": ["tempo", "stemsplit"], "out": "o"}, f)
        code, _, _ = run_cli("run", "--job", job)
        self.assertEqual(code, 3)
        with open(os.path.join(self.dir, "o/result.json")) as f:
            r = json.load(f)
        self.assertTrue(any(e.startswith("stemsplit:") for e in r["errors"]))


class PartProfileTests(unittest.TestCase):
    def test_parts_convert_like_their_instrument(self):
        self.assertIs(transcribe.profile("guitar_left"), transcribe.PROFILES["guitar"])
        self.assertIs(transcribe.profile("other_rest"), transcribe.PROFILES["other"])
        self.assertEqual(transcribe.profile("synth_pad")["program"], 89)
        self.assertIs(transcribe.profile("kazoo"), transcribe.PROFILES["other"])


if __name__ == "__main__":
    unittest.main()
