import functools
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from b4nd_analyzer import analyze, audio, synth  # noqa: E402


@functools.lru_cache(maxsize=None)
def analysed(bpm=128.0, numerator=4, lead_in=0.6, semitones=0, tempo_change=0.0, tasks=analyze.ANALYSIS_TASKS):
    """(song, truth, result) for a synthetic song, analysed through a WAV file like the app does."""
    import tempfile

    song = synth.demo_song(bpm, numerator, lead_in, sr=44100, semitones=semitones)
    if tempo_change:
        song.sections[3].bpm = tempo_change
        song.sections[4].bpm = tempo_change
    y = synth.render(song)
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "song.wav")
        synth.write_wav(path, y, song.sr, channels=2)
        result, errors = analyze.run(path, list(tasks))
    assert not errors, errors
    return song, song.truth(), result


def overlap_accuracy(truth_chords, chords, start, end):
    """Share of [start, end) where the analysed chord equals the true one."""
    import numpy as np

    t = np.arange(start, end, 0.05)

    def at(lst, x):
        for c in lst:
            if c["startSec"] <= x < c["endSec"]:
                return c["chord"]
        return None

    hits = [at(truth_chords, x) == at(chords, x) for x in t]
    return float(np.mean(hits))
