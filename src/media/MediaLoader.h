#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <deque>
#include <map>
#include <memory>

#include "engine/PlaybackPlan.h"

namespace b4nd
{

/**
    Decodes audio files completely into RAM at the engine sample rate, before playback
    (docs/03 §6). Nothing is read from disk while a song plays, so a slow or sleeping
    disk can never cause a dropout on stage.

    Supported: WAV, AIFF, FLAC, Ogg Vorbis, MP3. Files with more than two channels keep
    their first two. Sample-rate conversion uses a windowed-sinc interpolator.
*/
struct MediaDecodeResult
{
    std::shared_ptr<const AudioData> data;
    juce::String error;         // empty on success
    bool missing = false;       // file does not exist (moved / renamed / unplugged drive)
    double sourceSampleRate = 0.0;
    int sourceChannels = 0;
};

class MediaDecoder
{
public:
    MediaDecoder();

    /** Blocking decode. Safe to call from any non-realtime thread. */
    MediaDecodeResult decode (const juce::File& file, double targetSampleRate,
                              const std::atomic<bool>* cancel = nullptr);

    /** Decoded size in RAM (float, at most two channels, at the target rate) from the file
        header, without decoding. 0 when the file cannot be read. */
    std::int64_t estimateBytes (const juce::File& file, double targetSampleRate);

    /** "*.wav;*.aif;..." for file choosers and drag-and-drop filters. */
    juce::String getWildcard() const;
    bool canRead (const juce::File& file) const;

    /** Hard limit per file (decoded size), so one wrong file cannot exhaust memory. */
    static constexpr std::int64_t maxDecodedBytesPerFile = (std::int64_t) 2 * 1024 * 1024 * 1024;

private:
    juce::AudioFormatManager formats;
};

/**
    Background decoding with a cache keyed by (file, sample rate).

    request() never blocks the caller. Results are collected on the message thread with
    takeFinished(). Decoded data is shared (shared_ptr<const>) between songs that use the
    same file, and is released when no song references it (purgeUnused()).
*/
class MediaPool : private juce::Thread
{
public:
    MediaPool();
    ~MediaPool() override;

    struct Finished
    {
        juce::String path;
        double sampleRate = 0.0;
        MediaDecodeResult result;
    };

    /** Returns cached data immediately if available, otherwise queues a decode. */
    std::shared_ptr<const AudioData> request (const juce::File& file, double sampleRate);

    /** Completed decodes since the last call (message thread). */
    std::vector<Finished> takeFinished();

    bool isBusy() const;
    int getQueueLength() const;

    /** Bytes held by the cache (decoded float samples). */
    std::int64_t getCachedBytes() const;

    /** Drops cache entries nobody else holds (e.g. after a sample-rate change). */
    void purgeUnused();

    bool canRead (const juce::File& f) const { return decoder.canRead (f); }
    /** Message thread (own decoder; the decode thread is not touched). */
    std::int64_t estimateBytes (const juce::File& f, double sampleRate) { return probe.estimateBytes (f, sampleRate); }
    juce::String getWildcard() const { return decoder.getWildcard(); }

    static std::int64_t bytesOf (const AudioData& d) noexcept
    {
        return (std::int64_t) d.numChannels * d.numFrames * (std::int64_t) sizeof (float);
    }

private:
    void run() override;

    struct Key
    {
        juce::String path;
        std::int64_t rate;   // Hz, rounded
        bool operator< (const Key& o) const { return path != o.path ? path < o.path : rate < o.rate; }
    };
    static Key makeKey (const juce::File& f, double sr) { return { f.getFullPathName(), (std::int64_t) std::llround (sr) }; }

    MediaDecoder decoder;
    MediaDecoder probe;
    mutable juce::CriticalSection lock;
    std::map<Key, std::shared_ptr<const AudioData>> cache;
    std::deque<Key> queue;
    std::vector<Finished> finished;
    bool working = false;
    Key inFlight;   // being decoded right now (not in the queue any more)
    std::atomic<bool> cancelFlag { false };
    juce::WaitableEvent wake;
};

} // namespace b4nd
