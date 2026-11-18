import unittest

from tests.util import analysed, overlap_accuracy


class HarmonyTests(unittest.TestCase):
    def test_key_and_chords_a_minor(self):
        song, truth, r = analysed(128.0)
        self.assertEqual(r["key"]["value"], "A minor", r["key"])
        self.assertEqual(len(r["key"]["candidates"]), 3)
        acc = overlap_accuracy(truth["chords"], r["chords"], truth["beats"][0], truth["endSec"])
        self.assertGreater(acc, 0.9, acc)

    def test_key_and_spelling_follow_transposition(self):
        # +5 semitones: D minor, a flat key -> Bb not A#.
        song, truth, r = analysed(110.0, semitones=5)
        self.assertEqual(r["key"]["value"], "D minor", r["key"])
        names = {c["chord"] for c in r["chords"]}
        self.assertIn("Bb", names)
        self.assertNotIn("A#", names)
        flat = {"A#": "Bb"}
        tc = [dict(c, chord=flat.get(c["chord"], c["chord"])) for c in truth["chords"]]
        acc = overlap_accuracy(tc, r["chords"], truth["beats"][0], truth["endSec"])
        self.assertGreater(acc, 0.9, acc)

    def test_chord_confidences_are_probabilities(self):
        _, _, r = analysed(128.0)
        for c in r["chords"]:
            self.assertGreaterEqual(c["confidence"], 0.0)
            self.assertLessEqual(c["confidence"], 1.0)
            self.assertLess(c["startSec"], c["endSec"])


if __name__ == "__main__":
    unittest.main()
