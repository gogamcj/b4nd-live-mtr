#include "media/MediaLoader.h"

namespace b4nd
{

MediaDecoder::MediaDecoder()
{
    formats.registerBasicFormats();
}

juce::String MediaDecoder::getWildcard() const
{
    return formats.getWildcardForAllFormats();
}

bool MediaDecoder::canRead (const juce::File& file) const
{
    return formats.findFormatForFileExtension (file.getFileExtension()) != nullptr;
}

MediaDecodeResult MediaDecoder::decode (const juce::File& file, double targetSampleRate, const std::atomic<bool>* cancel)
{
    MediaDecodeResult r;
    if (! file.existsAsFile())
    {
        r.missing = true;
        r.error = "File not found: " + file.getFullPathName();
        return r;
    }
    if (targetSampleRate <= 0.0)
    {
        r.error = "Invalid target sample rate";
        return r;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        r.error = "Unsupported or damaged audio file: " + file.getFileName();
        return r;
    }

    r.sourceSampleRate = reader->sampleRate;
    r.sourceChannels = (int) reader->numChannels;
    const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
    const auto srcFrames = (std::int64_t) reader->lengthInSamples;

    if (srcFrames <= 0 || reader->sampleRate <= 0.0)
    {
        r.error = "Audio file is empty: " + file.getFileName();
        return r;
    }

    const double ratio = reader->sampleRate / targetSampleRate;   // input samples per output sample
    const auto outFrames = (std::int64_t) std::ceil ((double) srcFrames / ratio);
    if ((std::int64_t) channels * outFrames * (std::int64_t) sizeof (float) > maxDecodedBytesPerFile
        || srcFrames > (std::int64_t) std::numeric_limits<int>::max())
    {
        r.error = "Audio file too long to load into memory: " + file.getFileName();
        return r;
    }

    // 1. Read the whole file at its own rate.
    std::vector<std::vector<float>> src ((size_t) channels);
    try
    {
        for (auto& c : src)
            c.resize ((size_t) srcFrames);
    }
    catch (const std::bad_alloc&)
    {
        r.error = "Not enough memory to load " + file.getFileName();
        return r;
    }

    constexpr int chunk = 65536;
    for (std::int64_t pos = 0; pos < srcFrames; pos += chunk)
    {
        if (cancel != nullptr && cancel->load())
        {
            r.error = "Cancelled";
            return r;
        }
        const int n = (int) std::min<std::int64_t> (chunk, srcFrames - pos);
        float* dest[2] = { src[0].data() + pos, channels > 1 ? src[1].data() + pos : nullptr };
        if (! reader->read (dest, channels, pos, n))
        {
            r.error = "Read error in " + file.getFileName();
            return r;
        }
    }

    auto data = std::make_shared<AudioData>();
    data->numChannels = channels;

    // 2. Convert the sample rate if needed.
    if (std::abs (ratio - 1.0) < 1e-9)
    {
        data->numFrames = srcFrames;
        data->channels = std::move (src);
    }
    else
    {
        data->numFrames = outFrames;
        data->channels.resize ((size_t) channels);
        for (int c = 0; c < channels; ++c)
        {
            if (cancel != nullptr && cancel->load())
            {
                r.error = "Cancelled";
                return r;
            }
            auto& in = src[(size_t) c];
            // The sinc interpolator delays its output by a fixed number of input samples:
            // render that much extra and drop it from the front, so clips stay sample-aligned.
            const auto latencyOut = (std::int64_t) std::lround (juce::WindowedSincInterpolator::getBaseLatency() / ratio);
            in.resize (in.size() + (size_t) std::ceil ((double) (latencyOut + 64) * ratio) + 256, 0.0f);
            auto& out = data->channels[(size_t) c];
            out.assign ((size_t) (outFrames + latencyOut), 0.0f);
            juce::WindowedSincInterpolator interp;
            interp.process (ratio, in.data(), out.data(), (int) (outFrames + latencyOut));
            out.erase (out.begin(), out.begin() + latencyOut);
            std::vector<float>().swap (in);   // free as we go
        }
    }

    r.data = std::move (data);
    return r;
}

//==============================================================================
MediaPool::MediaPool() : juce::Thread ("B4ND media decoder")
{
    startThread (juce::Thread::Priority::low);
}

MediaPool::~MediaPool()
{
    cancelFlag = true;
    signalThreadShouldExit();
    wake.signal();
    stopThread (10000);
}

std::shared_ptr<const AudioData> MediaPool::request (const juce::File& file, double sampleRate)
{
    const auto key = makeKey (file, sampleRate);
    const juce::ScopedLock sl (lock);
    if (auto it = cache.find (key); it != cache.end())
        return it->second;

    const auto same = [&] (const Key& k) { return ! (k < key) && ! (key < k); };
    if ((! working || ! same (inFlight)) && std::find_if (queue.begin(), queue.end(), same) == queue.end())
        queue.push_back (key);
    wake.signal();
    return nullptr;
}

std::vector<MediaPool::Finished> MediaPool::takeFinished()
{
    const juce::ScopedLock sl (lock);
    std::vector<Finished> out;
    out.swap (finished);
    return out;
}

bool MediaPool::isBusy() const
{
    const juce::ScopedLock sl (lock);
    return working || ! queue.empty();
}

int MediaPool::getQueueLength() const
{
    const juce::ScopedLock sl (lock);
    return (int) queue.size() + (working ? 1 : 0);
}

std::int64_t MediaPool::getCachedBytes() const
{
    const juce::ScopedLock sl (lock);
    std::int64_t total = 0;
    for (const auto& [k, d] : cache)
        if (d != nullptr)
            total += bytesOf (*d);
    return total;
}

void MediaPool::purgeUnused()
{
    std::vector<std::shared_ptr<const AudioData>> toFree;   // freed outside the lock
    {
        const juce::ScopedLock sl (lock);
        for (auto it = cache.begin(); it != cache.end();)
        {
            if (it->second.use_count() == 1)
            {
                toFree.push_back (std::move (it->second));
                it = cache.erase (it);
            }
            else
                ++it;
        }
    }
}

void MediaPool::run()
{
    while (! threadShouldExit())
    {
        Key key;
        {
            const juce::ScopedLock sl (lock);
            if (! queue.empty())
            {
                key = queue.front();
                queue.pop_front();
                working = true;
                inFlight = key;
            }
        }

        if (key.path.isEmpty())
        {
            wake.wait (500);
            continue;
        }

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto result = decoder.decode (juce::File (key.path), (double) key.rate, &cancelFlag);
        if (result.data != nullptr)
            juce::Logger::writeToLog ("Media loaded: " + key.path + " (" + juce::String (result.sourceSampleRate, 0) + " Hz -> "
                                      + juce::String (key.rate) + " Hz, "
                                      + juce::String ((double) bytesOf (*result.data) / (1024.0 * 1024.0), 1, false) + " MB, "
                                      + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - t0)) + " ms)");
        else
            juce::Logger::writeToLog ("Media load failed: " + result.error);

        const juce::ScopedLock sl (lock);
        if (result.data != nullptr)
            cache[key] = result.data;
        finished.push_back ({ key.path, (double) key.rate, std::move (result) });
        working = false;
    }
}

} // namespace b4nd
