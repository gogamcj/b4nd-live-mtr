#include "plugins/PluginInstrument.h"

#include <chrono>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace b4nd
{

namespace
{
constexpr int faultCppException = 1;
constexpr int faultUnknown = 2;

int callProcess (juce::AudioProcessor& p, juce::AudioBuffer<float>& b, juce::MidiBuffer& m) noexcept
{
    try
    {
        p.processBlock (b, m);
        return 0;
    }
    catch (...)
    {
        return faultCppException;
    }
}

#if JUCE_WINDOWS && defined (_MSC_VER)
// No C++ objects with destructors in this frame (SEH and C++ unwinding cannot mix in one function).
int sehProcess (juce::AudioProcessor* p, juce::AudioBuffer<float>* b, juce::MidiBuffer* m) noexcept
{
    __try
    {
        return callProcess (*p, *b, *m);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        const auto code = GetExceptionCode();
        return code != 0 ? (int) code : faultUnknown;
    }
}
#endif
} // namespace

int guardedProcess (juce::AudioProcessor& p, juce::AudioBuffer<float>& b, juce::MidiBuffer& m) noexcept
{
   #if JUCE_WINDOWS && defined (_MSC_VER)
    return sehProcess (&p, &b, &m);
   #else
    return callProcess (p, b, m);
   #endif
}

PluginInstrument::PluginInstrument (std::unique_ptr<juce::AudioPluginInstance> p)
    : plugin (std::move (p))
{
    midi.ensureSize (16384);
    plugin->addListener (this);
}

PluginInstrument::~PluginInstrument()
{
    plugin->removeListener (this);
    const juce::ScopedLock sl (lock);
    if (plugin != nullptr && preparedRate > 0.0)
    {
        try { plugin->releaseResources(); } catch (...) {}
    }
}

bool PluginInstrument::prepare (double sampleRate, int maxBlockSize, juce::String& error)
{
    const juce::ScopedLock sl (lock);   // render() skips (silence) while we hold this
    try
    {
        if (preparedRate > 0.0)
            plugin->releaseResources();
        // Instruments: no audio input, main output stereo when the plugin allows it.
        auto layout = plugin->getBusesLayout();
        if (! layout.outputBuses.isEmpty())
        {
            layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            for (auto& in : layout.inputBuses)
                in = juce::AudioChannelSet::disabled();
            if (plugin->checkBusesLayoutSupported (layout))
                plugin->setBusesLayout (layout);
        }
        plugin->setRateAndBufferSizeDetails (sampleRate, maxBlockSize);
        plugin->prepareToPlay (sampleRate, maxBlockSize);
        const int channels = juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
        buffer.setSize (channels, maxBlockSize, false, true, false);
        midi.ensureSize (16384);
        preparedRate = sampleRate;
        preparedBlock = maxBlockSize;
        updateLatency();
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }
    catch (...)
    {
        error = "exception in prepareToPlay";
    }
    preparedRate = 0.0;
    preparedBlock = 0;
    return false;
}

bool PluginInstrument::isPreparedFor (double sampleRate, int maxBlockSize) const noexcept
{
    return std::abs (preparedRate - sampleRate) < 0.5 && preparedBlock >= maxBlockSize;
}

bool PluginInstrument::render (float* outL, float* outR, int n, const MidiBytes* msgs, const int* offsets, int count) noexcept
{
    auto silence = [&] { std::fill (outL, outL + n, 0.0f); std::fill (outR, outR + n, 0.0f); };
    if (failed.load (std::memory_order_relaxed))
        return false;

    const juce::ScopedTryLock tl (lock);
    if (! tl.isLocked() || preparedRate <= 0.0 || n > preparedBlock)
    {
        silence();   // being prepared on the message thread, or not ready yet
        return true;
    }

    midi.clear();
    for (int i = 0; i < count; ++i)
        midi.addEvent (msgs[i].data, (int) msgs[i].size, offsets[i]);   // capacity reserved in prepare()

    // A view of the first n samples of the preallocated buffer (no allocation).
    juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n);
    view.clear();

    const auto t0 = std::chrono::steady_clock::now();
    const int fault = guardedProcess (*plugin, view, midi);
    const auto t1 = std::chrono::steady_clock::now();
    if (fault != 0)
    {
        failureCode.store (fault, std::memory_order_relaxed);
        failed.store (true, std::memory_order_relaxed);
        silence();
        return false;
    }

    const double used = std::chrono::duration<double> (t1 - t0).count() * preparedRate / (double) n;
    load.store (load.load (std::memory_order_relaxed) * 0.95f + (float) used * 0.05f, std::memory_order_relaxed);

    const float* l = view.getReadPointer (0);
    const float* r = view.getNumChannels() > 1 ? view.getReadPointer (1) : l;
    for (int i = 0; i < n; ++i)
    {
        // A misbehaving plugin must never put NaN / Inf or a full-scale blast into the PA.
        const float a = l[i], b = r[i];
        outL[i] = std::isfinite (a) ? juce::jlimit (-4.0f, 4.0f, a) : 0.0f;
        outR[i] = std::isfinite (b) ? juce::jlimit (-4.0f, 4.0f, b) : 0.0f;
    }
    return true;
}

void PluginInstrument::updateLatency() noexcept
{
    // More than 1 s is not a real look-ahead: treat it as a bad report rather than shift the
    // MIDI a whole second.
    int l = 0;
    try { l = plugin->getLatencySamples(); } catch (...) { l = 0; }
    latency.store (juce::jlimit (0, preparedRate > 0.0 ? (int) preparedRate : 48000, l), std::memory_order_relaxed);
}

juce::String PluginInstrument::getFailure() const
{
    const int c = failureCode.load (std::memory_order_relaxed);
    if (c == 0)
        return {};
    if (c == faultCppException)
        return "exception in the plugin's audio processing";
    return "crash in the plugin's audio processing (code 0x" + juce::String::toHexString (c) + ")";
}

juce::MemoryBlock PluginInstrument::getState()
{
    juce::MemoryBlock mb;
    try { plugin->getStateInformation (mb); } catch (...) { mb.reset(); }
    return mb;
}

bool PluginInstrument::setState (const juce::MemoryBlock& mb)
{
    if (mb.isEmpty())
        return true;
    try
    {
        plugin->setStateInformation (mb.getData(), (int) mb.getSize());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace b4nd
