import unittest

import numpy as np

from tests.util import analysed, audio, synth
from b4nd_analyzer import tempo


class TempoTests(unittest.TestCase):
    def check_song(self, bpm, numerator=4, lead_in=0.6):
        song, truth, r = analysed(bpm, numerator, lead_in)
        self.assertAlmostEqual(r["bpm"]["value"], bpm, delta=0.05, msg=r["bpm"])
        self.assertGreaterEqual(r["bpm"]["confidence"], 0.7)
        self.assertEqual(r["timeSig"], f"{numerator}/4")
        # Bar 1 beat 1 lands on the first downbeat (the grid the click will follow).
        self.assertAlmostEqual(r["gridStartSec"], lead_in, delta=0.010)
        self.assertEqual(len(r["tempoMap"]), 1, r["tempoMap"])
        self.assertFalse(r["variableTempo"])
        # The grid stays on the beat to the end of the song (no drift).
        last = truth["beats"][-1]
        n = round((last - r["gridStartSec"]) * r["bpm"]["value"] / 60.0)
        grid_last = r["gridStartSec"] + n * 60.0 / r["tempoMap"][0]["bpm"]
        self.assertAlmostEqual(grid_last, last, delta=0.010)

    def test_common_tempos_4_4(self):
        for bpm, lead in ((92.0, 1.3), (128.0, 0.6), (150.0, 0.25)):
            with self.subTest(bpm=bpm):
                self.check_song(bpm, 4, lead)

    def test_three_four(self):
        self.check_song(140.0, 3, 0.4)

    def test_meter_hint_keeps_the_users_beats_per_bar(self):
        # The user says "3 beats per bar": only the downbeat is searched (3/4 is never voted down).
        import tempfile, os
        from b4nd_analyzer import analyze
        for numerator, lead in ((3, 0.4), (4, 0.6)):
            with self.subTest(numerator=numerator):
                song = synth.demo_song(120.0, numerator, lead, sr=44100)
                y = synth.render(song)
                with tempfile.TemporaryDirectory() as d:
                    path = os.path.join(d, "song.wav")
                    synth.write_wav(path, y, song.sr, channels=2)
                    r, errors = analyze.run(path, ["tempo"], meter_hint=numerator)
                self.assertFalse(errors, errors)
                self.assertEqual(r["timeSig"], f"{numerator}/4")
                self.assertEqual(r["meterHint"], numerator)
                self.assertAlmostEqual(r["gridStartSec"], lead, delta=0.010)
        # A wrong hint is still obeyed (the user decides), the downbeat stays on a beat.
        song = synth.demo_song(120.0, 4, 0.6, sr=44100)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "song.wav")
            synth.write_wav(path, synth.render(song), song.sr, channels=2)
            r, _ = analyze.run(path, ["tempo"], meter_hint=3)
            bad, _ = analyze.run(path, ["tempo"], meter_hint=99)   # out of range: ignored
        self.assertEqual(r["timeSig"], "3/4")
        self.assertEqual(bad["timeSig"], "4/4")
        self.assertNotIn("meterHint", bad)

    def test_lead_in_of_several_seconds(self):
        self.check_song(100.0, 4, 3.1)

    def test_tempo_change_becomes_a_tempo_map(self):
        song, truth, r = analysed(120.0, 4, 0.5, tempo_change=126.0)
        pts = r["tempoMap"]
        self.assertTrue(r["variableTempo"])
        self.assertEqual(len(pts), 3, pts)
        # Verse 2 starts at bar 21 (beat 80) and the outro at bar 37 (beat 144).
        self.assertLessEqual(abs(pts[1]["beat"] - 80), 1, pts)
        self.assertLessEqual(abs(pts[2]["beat"] - 144), 1, pts)
        self.assertAlmostEqual(pts[0]["bpm"], 120.0, delta=0.3)
        self.assertAlmostEqual(pts[1]["bpm"], 126.0, delta=0.3)
        self.assertAlmostEqual(pts[2]["bpm"], 120.0, delta=0.3)
        # Times follow from beats and BPMs alone (how the MTR rebuilds the map).
        for a, b in zip(pts, pts[1:]):
            self.assertAlmostEqual(b["timeSec"], a["timeSec"] + (b["beat"] - a["beat"]) * 60 / a["bpm"], delta=1e-3)
        # The grid at the outro start is on the true downbeat.
        outro = truth["sections"][5]["startSec"]
        self.assertAlmostEqual(pts[2]["timeSec"], outro, delta=0.03)

    def test_octave_ambiguity_offers_the_true_tempo(self):
        # 174 BPM with 8th-note hats reads as 87: the alternatives must contain the truth.
        song, truth, r = analysed(174.0, 4, 0.25)
        vals = [r["bpm"]["value"]] + r["bpm"]["alternatives"]
        self.assertTrue(any(abs(v - 174.0) < 0.1 for v in vals), r["bpm"])

    def test_onset_timing_on_clicks(self):
        sr = audio.SR
        rng = np.random.default_rng(0)
        y = rng.standard_normal(sr * 20) * 1e-4
        ts = np.arange(0.5, 19.0, 0.5)
        for t in ts:
            i = int(t * sr)
            y[i:i + 400] += rng.standard_normal(400) * np.exp(-np.arange(400) / 80)
        f = audio.compute_features(y.astype(np.float32))
        r = tempo.analyse_tempo(f)
        self.assertAlmostEqual(r.bpm, 120.0, delta=0.02)
        err = [abs(x - ts[np.argmin(np.abs(ts - x))]) for x in r.beats]
        self.assertLess(np.median(err), 0.003)
        # Clicks have no bar accent, so bar 1 may start on any click, but exactly on one.
        self.assertLess(np.min(np.abs(ts - r.grid_start)), 0.004)

    def test_silence_and_noise_are_rejected_not_guessed(self):
        sr = audio.SR
        for name, y in (("silence", np.zeros(sr * 10, np.float32)),
                        ("short", synth.render(synth.demo_song(120))[: int(sr * 2)])):
            with self.subTest(name):
                with self.assertRaises(ValueError):
                    tempo.analyse_tempo(audio.compute_features(y))


if __name__ == "__main__":
    unittest.main()
