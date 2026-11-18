// AnalyzerRunner (docs/07 §4): result parsing, the real analyzer end to end, song-time
// conversion, and every way a run can go wrong (missing analyzer, crash, garbage, hang, cancel).

#include "TestHarness.h"
#include "app/AnalyzerRunner.h"

#include <cmath>
#include <cstdlib>

using namespace b4nd;

namespace
{
struct TempDir
{
    TempDir() : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("b4nd-analysis-test-" + juce::String (juce::Random::getSystemRandom().nextInt64())))
    {
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }
    juce::File dir;
};

void setEnv (const char* name, const juce::String& value)
{
#if JUCE_WINDOWS
    _putenv_s (name, value.toRawUTF8());
#else
    if (value.isEmpty())
        unsetenv (name);
    else
        setenv (name, value.toRawUTF8(), 1);
#endif
}

/** Clicks at `bpm` from `firstBeat` s, with an accent (louder, lower) every 4th beat. */
std::shared_ptr<AudioData> clickTrack (double sr, double seconds, double bpm, double firstBeat)
{
    auto d = std::make_shared<AudioData>();
    d->numChannels = 2;
    d->numFrames = (std::int64_t) (sr * seconds);
    d->channels.assign (2, std::vector<float> ((size_t) d->numFrames, 0.0f));
    juce::Random rng (7);
    int k = 0;
    for (double t = firstBeat; t < seconds - 0.2; t += 60.0 / bpm, ++k)
    {
        const bool accent = k % 4 == 0;
        const auto at = (std::int64_t) std::llround (t * sr);
        for (int i = 0; i < (int) (0.08 * sr) && at + i < d->numFrames; ++i)
        {
            const double env = std::exp (-i / (0.012 * sr));
            const float v = (float) (env * (accent ? 0.8 * std::sin (2.0 * juce::MathConstants<double>::pi * 70.0 * i / sr)
                                                     + 0.3 * (rng.nextFloat() * 2.0f - 1.0f)
                                                   : 0.4 * (rng.nextFloat() * 2.0f - 1.0f)));
            d->channels[0][(size_t) (at + i)] += v;
            d->channels[1][(size_t) (at + i)] += v;
        }
    }
    return d;
}

AnalyzerRunner::Status waitDone (AnalyzerRunner& r, double maxSeconds)
{
    const auto start = juce::Time::getMillisecondCounterHiRes();
    while (r.isBusy() && juce::Time::getMillisecondCounterHiRes() - start < maxSeconds * 1000.0)
    {
        r.getStatus();   // the status poll is also what enforces the timeout
        juce::Thread::sleep (50);
    }
    juce::Thread::sleep (100);
    return r.getStatus();
}

bool analyzerAvailable()
{
    juce::String err;
    const auto info = AnalyzerRunner::probe ({}, err);
    if (info.isEmpty())
    {
        std::printf ("  (analyzer not available: %s)\n", err.toRawUTF8());
        return false;
    }
    return true;
}

bool required()
{
    return juce::SystemStats::getEnvironmentVariable ("B4ND_REQUIRE_ANALYZER", {}) == "1";
}

/** A fake analyzer package: its "run" misbehaves in the way the job's first task names. */
juce::File writeFakePackage (const juce::File& dir)
{
    auto pkg = dir.getChildFile ("fake").getChildFile ("b4nd_analyzer");
    pkg.createDirectory();
    pkg.getChildFile ("__init__.py").replaceWithText ("");
    pkg.getChildFile ("cli.py").replaceWithText (R"(import json, os, sys, time
def main():
    a = sys.argv[1:]
    if a[0] == "info":
        print(json.dumps({"b4nd_analyzer": "fake", "python": "x", "numpy": "x", "scipy": "x"})); return 0
    job = json.load(open(a[2]))
    out = os.path.dirname(a[2])
    mode = job["tasks"][0]
    print(json.dumps({"task": "load", "progress": 0.5}), flush=True)
    if mode == "crash":
        print(json.dumps({"error": "fake crash"}), flush=True); return 2
    if mode == "hang":
        time.sleep(120); return 0
    if mode == "garbage":
        open(os.path.join(out, "result.json"), "w").write("{not json"); return 0
    if mode == "nonsense":
        json.dump({"status": "done", "errors": [], "bpm": {"value": "fast", "confidence": 7},
                   "tempoMap": [{"beat": -4, "bpm": 120}, {"beat": 0, "bpm": 0}], "timeSig": "banana",
                   "key": {"value": "", "candidates": []},
                   "chords": [{"startSec": 5, "endSec": 2, "chord": "C"}, {"startSec": 1, "endSec": 2, "chord": "Am", "confidence": 0.5}],
                   "sections": [{"label": "X", "section": "drop", "startSec": 3.0, "bar": 1}]},
                  open(os.path.join(out, "result.json"), "w"))
        return 0
    return 9
)");
    return dir.getChildFile ("fake");
}

AnalyzerRunner::Job makeJob (const juce::File& out, juce::StringArray tasks = { "tempo", "key", "chords", "structure" })
{
    AnalyzerRunner::Job j;
    j.songId = "song-1";
    j.outDir = out;
    j.tasks = tasks;
    j.media = clickTrack (44100.0, 6.0, 120.0, 0.5);
    j.mediaRate = 44100.0;
    return j;
}
} // namespace

TEST_CASE ("Analysis: result.json is parsed, and bad values are dropped instead of trusted")
{
    const auto r = AnalysisResult::parse (R"({"status":"partial","errors":["stems: later"],"durationSec":180.5,
        "bpm":{"value":128.004,"confidence":0.93,"alternatives":[64.0,256.0]},"gridStartSec":0.604,
        "tempoMap":[{"beat":0,"bpm":128.004,"timeSec":0.604},{"beat":80,"bpm":126.0,"timeSec":38.1}],"variableTempo":true,
        "timeSig":"3/4","timeSigConfidence":0.8,
        "key":{"value":"D minor","confidence":0.41,"candidates":[{"value":"D minor"},{"value":"F major"},{"value":"A minor"}]},
        "chords":[{"startSec":2.5,"endSec":4.0,"chord":"Bb","confidence":0.7},{"startSec":0.6,"endSec":2.5,"chord":"Dm","confidence":0.9}],
        "sections":[{"label":"Intro","section":"intro","startSec":0.6,"bar":0,"confidence":0.4},
                    {"label":"Drop","section":"drop","startSec":20.0,"bar":8}],
        "versions":{"b4nd_analyzer":"0.1.0"}})");
    CHECK (r.valid);
    CHECK (r.status == "partial");
    CHECK (r.errors.size() == 1);
    CHECK (r.hasTempo);
    CHECK_NEAR (r.bpm, 128.004, 1e-9);
    CHECK_NEAR (r.gridStartSec, 0.604, 1e-9);
    CHECK (r.tempoMap.size() == 2);
    CHECK (r.variableTempo);
    CHECK (r.numerator == 3);
    CHECK (r.key == "D minor");
    CHECK (r.keyCandidates.size() == 3);
    CHECK (r.chords.size() == 2);
    if (r.chords.size() == 2)
        CHECK (r.chords[0].chord == "Dm");   // sorted by time
    CHECK (r.sections.size() == 2);
    if (r.sections.size() == 2)
        CHECK (r.sections[1].section == "custom");   // not a cue_point section: stored as custom
    CHECK (r.versionsJson.contains ("0.1.0"));

    CHECK (! AnalysisResult::parse ("{not json").valid);
    CHECK (! AnalysisResult::parse (R"({"status":"failed","errors":["input not found"]})").valid);
    const auto n = AnalysisResult::parse (R"({"status":"done","bpm":{"value":-5},"tempoMap":[{"beat":0,"bpm":1e9}]})");
    CHECK (n.valid);
    CHECK (! n.hasTempo);
}

TEST_CASE ("Analysis: the real analyzer finds tempo and bar 1 in song time")
{
    if (! analyzerAvailable())
    {
        CHECK (! required());
        return;
    }
    TempDir t;
    AnalyzerRunner r;
    auto job = makeJob (t.dir.getChildFile ("analysis/song-1"), { "tempo", "key" });
    job.media = clickTrack (44100.0, 20.0, 120.0, 0.5);
    job.mediaStartSec = 0.25;   // the track starts 0.25 s into the song: bar 1 must be at 0.75 s
    CHECK (r.start (job).isEmpty());
    CHECK (r.start (job).isNotEmpty());   // one run at a time
    const auto st = waitDone (r, 120.0);
    CHECK (st.state == AnalyzerRunner::State::Done);
    if (st.state != AnalyzerRunner::State::Done)
        std::printf ("  state %d: %s\n", (int) st.state, st.message.toRawUTF8());
    CHECK_NEAR (st.progress, 1.0f, 1e-6f);
    const auto res = r.getResult();
    CHECK (res.valid);
    CHECK_NEAR (res.bpm, 120.0, 0.05);
    CHECK (res.numerator == 4);
    CHECK_NEAR (res.gridStartSec, 0.75, 0.01);
    CHECK (res.hasKey);
    CHECK (! res.hasChords);   // not asked for
    CHECK (t.dir.getChildFile ("analysis/song-1/result.json").existsAsFile());
    CHECK (! t.dir.getChildFile ("analysis/song-1/input.wav").exists());   // removed after the run
}

TEST_CASE ("Analysis: failures are reported, never hang, and leave no running process")
{
    if (! analyzerAvailable())
    {
        CHECK (! required());
        return;
    }
    TempDir t;
    setEnv ("B4ND_ANALYZER_DIR", writeFakePackage (t.dir).getFullPathName());
    {
        AnalyzerRunner r;
        CHECK (r.start (makeJob (t.dir.getChildFile ("a"), { "crash" })).isEmpty());
        auto st = waitDone (r, 30.0);
        CHECK (st.state == AnalyzerRunner::State::Failed);
        CHECK (st.message.contains ("fake crash"));
        CHECK (st.exitCode == 2);
        if (st.exitCode != 2) std::printf ("  crash: exit %d, %s\n", st.exitCode, st.message.toRawUTF8());

        CHECK (r.start (makeJob (t.dir.getChildFile ("b"), { "garbage" })).isEmpty());
        st = waitDone (r, 30.0);
        CHECK (st.state == AnalyzerRunner::State::Failed);
        CHECK (st.message.contains ("JSON"));

        CHECK (r.start (makeJob (t.dir.getChildFile ("c"), { "nonsense" })).isEmpty());
        st = waitDone (r, 30.0);
        CHECK (st.state == AnalyzerRunner::State::Done);
        if (st.state != AnalyzerRunner::State::Done) std::printf ("  nonsense: exit %d, %s\n", st.exitCode, st.message.toRawUTF8());
        const auto res = r.getResult();
        CHECK (! res.hasTempo);             // NaN BPM, 0 BPM and negative beats are not a tempo
        CHECK (res.chords.size() == 1);     // the backwards chord is dropped
        CHECK (res.sections.size() == 1 && res.sections[0].section == "custom");

        // Cancel: the process is killed at once.
        CHECK (r.start (makeJob (t.dir.getChildFile ("d"), { "hang" })).isEmpty());
        juce::Thread::sleep (1500);
        CHECK (r.getStatus().state == AnalyzerRunner::State::Running);
        const auto c0 = juce::Time::getMillisecondCounterHiRes();
        r.cancel();
        st = waitDone (r, 10.0);
        CHECK (st.state == AnalyzerRunner::State::Cancelled);
        CHECK (juce::Time::getMillisecondCounterHiRes() - c0 < 5000.0);

        // Timeout: a hung analyzer is killed.
        r.setTimeoutSeconds (2.0);
        CHECK (r.start (makeJob (t.dir.getChildFile ("e"), { "hang" })).isEmpty());
        st = waitDone (r, 20.0);
        CHECK (st.state == AnalyzerRunner::State::Cancelled);
        CHECK (st.message == "Timed out");
    }
    setEnv ("B4ND_ANALYZER_DIR", {});

    // An analyzer path that does not exist: a clear failure, not a hang.
    AnalyzerRunner r;
    r.setCommand (t.dir.getChildFile ("no-such-analyzer").getFullPathName());
    juce::String why;
    CHECK (AnalyzerRunner::probe (r.getCommand(), why).isEmpty());
    CHECK (why.contains ("NOT FOUND"));
    CHECK (r.start (makeJob (t.dir.getChildFile ("f"))).isEmpty());
    const auto st = waitDone (r, 10.0);
    CHECK (st.state == AnalyzerRunner::State::Failed);
    CHECK (st.message.contains ("NOT FOUND"));

    // Nothing to analyse.
    auto empty = makeJob (t.dir.getChildFile ("g"));
    empty.media.reset();
    CHECK (r.start (empty).isNotEmpty());
}

TEST_CASE ("Analysis: stem entries are checked before their paths are used")
{
    const auto r = AnalysisResult::parse (R"({"status":"done","errors":[],
        "stems":{"model":"htdemucs_6s","residualDb":-35.0,"warnings":["piano: check"],
                 "files":[{"name":"vocals","path":"stems/vocals.wav","energyDb":-6.5},
                          {"name":"piano","path":"stems/piano.wav","energyDb":-60.0,"empty":true},
                          {"name":"evil","path":"../../evil.wav"},
                          {"name":"../x","path":"stems/../x.wav"}]},
        "job":{"sourceTrackId":"track-7","sourceStartSec":1.25}})");
    CHECK (r.valid);
    CHECK (r.hasStems);
    CHECK (r.stems.size() == 2);   // the two path tricks are ignored
    CHECK (r.errors.size() == 2);
    CHECK (r.stems.size() == 2 && r.stems[1].empty);
    CHECK_NEAR (r.stemResidualDb, -35.0, 1e-9);
    CHECK (r.stemConfidence > 0.7);
    CHECK (r.stemWarnings.size() == 1);
    CHECK (r.sourceTrackId == "track-7");
    CHECK_NEAR (r.sourceStartSec, 1.25, 1e-9);

    const auto bad = AnalysisResult::parse (R"({"status":"done","stems":{"residualDb":-8,"files":[{"name":"bass","path":"stems/bass.wav"}]}})");
    CHECK_NEAR (bad.stemConfidence, 0.3, 1e-9);   // stems that do not add up to the mix: low
}

TEST_CASE ("Analysis: a stem run writes stereo input, gets the stems back and cleans up")
{
    if (! analyzerAvailable())
    {
        CHECK (! required());
        return;
    }
    TempDir t;
    setEnv ("B4ND_ANALYZER_FAKE_STEMS", "1");   // band-split stand-in: the app side, not Demucs
    AnalyzerRunner r;
    auto job = makeJob (t.dir.getChildFile ("analysis/song-1"), { "stems" });
    job.media = clickTrack (48000.0, 8.0, 120.0, 0.5);
    job.mediaRate = 48000.0;
    job.mediaStartSec = 2.0;
    job.sourceTrackId = "track-original";
    t.dir.getChildFile ("analysis/song-1/stems").createDirectory();
    t.dir.getChildFile ("analysis/song-1/stems/old.wav").replaceWithText ("old run");
    CHECK (r.start (job).isEmpty());
    const auto st = waitDone (r, 120.0);
    setEnv ("B4ND_ANALYZER_FAKE_STEMS", {});
    CHECK (st.state == AnalyzerRunner::State::Done);
    if (st.state != AnalyzerRunner::State::Done)
        std::printf ("  state %d: %s\n", (int) st.state, st.message.toRawUTF8());
    const auto res = r.getResult();
    CHECK (res.hasStems);
    CHECK (res.stems.size() == 6);
    CHECK (! res.hasTempo);   // only stems were asked for
    CHECK (res.sourceTrackId == "track-original");
    CHECK_NEAR (res.sourceStartSec, 2.0, 1e-9);
    CHECK (res.stemResidualDb < -60.0);   // the band split adds up exactly
    const auto dir = t.dir.getChildFile ("analysis/song-1");
    for (const auto& s : res.stems)
    {
        const auto f = dir.getChildFile (s.relPath);
        CHECK (f.existsAsFile());
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (f.createInputStream().release(), true));
        CHECK (rd != nullptr);
        if (rd != nullptr)
        {
            CHECK (rd->numChannels == 2);
            CHECK_NEAR ((double) rd->lengthInSamples / rd->sampleRate, 8.0, 0.001);   // media time: same length as the source
        }
    }
    CHECK (! dir.getChildFile ("stems/old.wav").exists());     // the previous run's stems are replaced
    CHECK (! dir.getChildFile ("stems_input.wav").exists());   // removed after the run
    CHECK (! dir.getChildFile ("input.wav").exists());
}

TEST_CASE ("Analysis: MIDI parts are parsed, and impossible notes are dropped")
{
    const auto r = AnalysisResult::parse (R"({"status":"done","errors":[],
        "midi":{"tracks":[
          {"name":"bass","sourceId":"track-9","program":33,"drums":false,"confidence":0.71,
           "notes":[[0.5,0.9,40,90,0.8],[1.0,0.9,40,90,0.8],[1.0,1.4,200,90,0.8],[1.5,1.9,41,0,0.5],[2.0,2.4,43,100,0.6],"x",[3.0]]},
          {"name":"drums","sourceId":"stems/drums","drums":true,"confidence":0.9,"notes":[[0.1,0.16,36,110,0.9]]},
          {"name":"../evil","notes":[]}]}})");
    CHECK (r.valid);
    CHECK (r.hasMidi);
    CHECK (r.midi.size() == 2);
    if (r.midi.size() == 2)
    {
        CHECK (r.midi[0].notes.size() == 2);   // backwards, pitch 200, velocity 0 and junk are dropped
        CHECK (r.midi[0].sourceId == "track-9");
        CHECK (r.midi[0].program == 33);
        CHECK (r.midi[1].drums);
        CHECK (r.midi[1].sourceId == "stems/drums");
    }
}

TEST_CASE ("Analysis: an audio-to-MIDI run needs no track audio, only its input files")
{
    if (! analyzerAvailable())
    {
        CHECK (! required());
        return;
    }
    TempDir t;
    // A drum stem (clicks at 120 BPM, a low accent every 4th): drums need no model, so this runs everywhere.
    auto drums = clickTrack (44100.0, 6.0, 120.0, 0.5);
    const auto wavFile = t.dir.getChildFile ("drums.wav");
    {
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::OutputStream> os (wavFile.createOutputStream().release());
        auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
        juce::AudioBuffer<float> b (2, (int) drums->numFrames);
        for (int c = 0; c < 2; ++c)
            b.copyFrom (c, 0, drums->channels[(size_t) c].data(), (int) drums->numFrames);
        CHECK (w != nullptr && w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()));
    }
    AnalyzerRunner r;
    AnalyzerRunner::Job job;
    job.songId = "song-1";
    job.outDir = t.dir.getChildFile ("analysis/song-1");
    job.tasks = { "midi" };
    CHECK (r.start (job).isNotEmpty());   // nothing to convert
    job.midiInputs.push_back ({ "drums", wavFile.getFullPathName(), "track-drums" });
    CHECK (r.start (job).isEmpty());
    const auto st = waitDone (r, 60.0);
    CHECK (st.state == AnalyzerRunner::State::Done);
    if (st.state != AnalyzerRunner::State::Done)
        std::printf ("  state %d: %s\n", (int) st.state, st.message.toRawUTF8());
    const auto res = r.getResult();
    CHECK (res.hasMidi);
    if (res.hasMidi)
    {
        CHECK (res.midi[0].drums);
        CHECK (res.midi[0].sourceId == "track-drums");
        int kicks = 0;
        for (const auto& n : res.midi[0].notes)
            if (n.pitch == 36) ++kicks;
        CHECK (res.midi[0].notes.size() >= 8);   // 11 clicks from 0.5 s at 120 BPM
        CHECK (kicks >= 1);                       // a low accent (the drum classifier itself is tested in Python)
        std::printf ("  drums: %d notes, %d kicks\n", (int) res.midi[0].notes.size(), kicks);
    }
    CHECK (! job.outDir.getChildFile ("input.wav").exists());   // no track audio was written
}
