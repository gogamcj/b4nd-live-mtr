import unittest

from tests.util import analysed


class StructureTests(unittest.TestCase):
    def check(self, bpm, numerator=4):
        song, truth, r = analysed(bpm, numerator)
        got = r["sections"]
        want = truth["sections"]
        self.assertEqual([s["section"] for s in got], [s["label"] for s in want], got)
        bar = numerator * 60.0 / bpm
        for g, w in zip(got, want):
            self.assertLess(abs(g["startSec"] - w["startSec"]), bar * 0.5, (g, w))
        self.assertEqual([s["label"] for s in got], ["Intro", "Verse 1", "Chorus 1", "Verse 2", "Chorus 2", "Outro"])

    def test_sections_4_4(self):
        self.check(128.0)

    def test_sections_slow(self):
        self.check(92.0)


if __name__ == "__main__":
    unittest.main()
