// Media decoding tests: formats, sample-rate conversion, alignment, missing/damaged files.

#include "TestHarness.h"
#include "media/MediaLoader.h"

#include <cmath>

using namespace b4nd;

namespace
{
struct TempDir
{
    TempDir() : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("b4nd-media-test-" + juce::String (juce::Random::getSystemRandom().nextInt64())))
    {
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }
    juce::File dir;
};

/** Writes a file: silence, then a 1 kHz sine starting at `onset`, `seconds` long. */
juce::File writeTestFile (const juce::File& f, juce::AudioFormat& format, double sr, int channels, double seconds, int onset, int bits = 24)
{
    const int n = (int) (sr * seconds);
    juce::AudioBuffer<float> buf (channels, n);
    buf.clear();
    for (int c = 0; c < channels; ++c)
        for (int i = onset; i < n; ++i)
            buf.setSample (c, i, (c == 0 ? 0.5f : 0.25f) * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (i - onset) / sr));

    f.deleteFile();
    std::unique_ptr<juce::OutputStream> os = f.createOutputStream();
    auto w = format.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (sr)
                                             .withNumChannels (channels).withBitsPerSample (bits));
    if (w != nullptr)
        w->writeFromAudioSampleBuffer (buf, 0, n);
    return f;
}

int firstAbove (const std::vector<float>& v, float t)
{
    for (int i = 0; i < (int) v.size(); ++i)
        if (std::abs (v[(size_t) i]) > t)
            return i;
    return -1;
}

double rms (const std::vector<float>& v, size_t from, size_t to)
{
    double e = 0;
    for (size_t i = from; i < to; ++i) e += (double) v[i] * v[i];
    return std::sqrt (e / (double) (to - from));
}

/** Zero crossings per second -> frequency estimate. */
double frequency (const std::vector<float>& v, size_t from, size_t to, double sr)
{
    int crossings = 0;
    for (size_t i = from + 1; i < to; ++i)
        if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++crossings;
    return crossings / 2.0 / ((double) (to - from) / sr);
}
} // namespace

TEST_CASE ("Media: WAV at the device rate loads bit-exact length and channels")
{
    TempDir t;
    juce::WavAudioFormat wav;
    auto f = writeTestFile (t.dir.getChildFile ("a.wav"), wav, 48000, 2, 1.0, 4800);
    MediaDecoder d;
    auto r = d.decode (f, 48000);
    CHECK (r.error.isEmpty());
    CHECK (r.data != nullptr);
    if (r.data == nullptr) return;
    CHECK (r.data->numChannels == 2);
    CHECK (r.data->numFrames == 48000);
    CHECK (firstAbove (r.data->channels[0], 0.01f) >= 4800 && firstAbove (r.data->channels[0], 0.01f) < 4810);
    CHECK_NEAR (rms (r.data->channels[0], 10000, 40000), 0.5 / std::sqrt (2.0), 0.005);
    CHECK_NEAR (rms (r.data->channels[1], 10000, 40000), 0.25 / std::sqrt (2.0), 0.005);
}

TEST_CASE ("Media: 44.1 kHz file converted to 48 kHz keeps pitch, length and onset")
{
    TempDir t;
    juce::WavAudioFormat wav;
    auto f = writeTestFile (t.dir.getChildFile ("b.wav"), wav, 44100, 1, 2.0, 22050);   // onset at 0.5 s
    MediaDecoder d;
    auto r = d.decode (f, 48000);
    CHECK (r.data != nullptr);
    if (r.data == nullptr) return;
    CHECK_NEAR (r.sourceSampleRate, 44100.0, 1e-6);
    CHECK (std::abs (r.data->numFrames - 96000) <= 1);
    const int on = firstAbove (r.data->channels[0], 0.05f);
    CHECK (std::abs (on - 24000) < 12);                                 // within 0.25 ms
    CHECK_NEAR (frequency (r.data->channels[0], 30000, 90000, 48000), 1000.0, 2.0);
    CHECK_NEAR (rms (r.data->channels[0], 30000, 90000), 0.5 / std::sqrt (2.0), 0.01);
}

TEST_CASE ("Media: 96 kHz file converted down to 48 kHz")
{
    TempDir t;
    juce::WavAudioFormat wav;
    auto f = writeTestFile (t.dir.getChildFile ("c.wav"), wav, 96000, 2, 1.0, 0);
    MediaDecoder d;
    auto r = d.decode (f, 48000);
    CHECK (r.data != nullptr);
    if (r.data == nullptr) return;
    CHECK (std::abs (r.data->numFrames - 48000) <= 1);
    CHECK_NEAR (frequency (r.data->channels[0], 4800, 43200, 48000), 1000.0, 2.0);
}

TEST_CASE ("Media: AIFF and FLAC decode")
{
    TempDir t;
    juce::AiffAudioFormat aiff;
    juce::FlacAudioFormat flac;
    MediaDecoder d;
    auto a = d.decode (writeTestFile (t.dir.getChildFile ("d.aiff"), aiff, 48000, 2, 0.5, 0), 48000);
    auto b = d.decode (writeTestFile (t.dir.getChildFile ("e.flac"), flac, 48000, 2, 0.5, 0, 24), 48000);
    CHECK (a.data != nullptr && a.data->numFrames == 24000);
    CHECK (b.data != nullptr && b.data->numFrames == 24000);
    CHECK (d.canRead (juce::File ("x.mp3")));
    CHECK (d.canRead (juce::File ("x.WAV")));
    CHECK (! d.canRead (juce::File ("x.txt")));
}

TEST_CASE ("Media: missing and damaged files are reported, never crash")
{
    TempDir t;
    MediaDecoder d;
    auto missing = d.decode (t.dir.getChildFile ("nope.wav"), 48000);
    CHECK (missing.missing);
    CHECK (missing.data == nullptr);

    auto junk = t.dir.getChildFile ("junk.wav");
    junk.replaceWithText ("this is not audio");
    auto damaged = d.decode (junk, 48000);
    CHECK (! damaged.missing);
    CHECK (damaged.data == nullptr);
    CHECK (damaged.error.isNotEmpty());
}

TEST_CASE ("Media: more than two channels keeps the first two")
{
    TempDir t;
    juce::WavAudioFormat wav;
    auto f = writeTestFile (t.dir.getChildFile ("six.wav"), wav, 48000, 6, 0.2, 0);
    MediaDecoder d;
    auto r = d.decode (f, 48000);
    CHECK (r.data != nullptr && r.data->numChannels == 2);
    CHECK (r.sourceChannels == 6);
}

TEST_CASE ("Media: pool decodes in the background, caches, and purges")
{
    TempDir t;
    juce::WavAudioFormat wav;
    auto f = writeTestFile (t.dir.getChildFile ("p.wav"), wav, 48000, 2, 0.5, 0);
    MediaPool pool;
    CHECK (pool.request (f, 48000) == nullptr);   // queued
    std::vector<MediaPool::Finished> done;
    for (int i = 0; i < 500 && done.empty(); ++i)
    {
        juce::Thread::sleep (10);
        done = pool.takeFinished();
    }
    CHECK (done.size() == 1);
    auto cached = pool.request (f, 48000);
    CHECK (cached != nullptr);
    CHECK (pool.getCachedBytes() == 2 * 24000 * 4);
    pool.purgeUnused();
    CHECK (pool.getCachedBytes() > 0);             // still referenced by `cached`
    cached.reset();
    for (auto& d : done) d.result.data.reset();
    pool.purgeUnused();
    CHECK (pool.getCachedBytes() == 0);
}
