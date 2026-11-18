#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

#include "engine/PlaybackPlan.h"

namespace b4nd
{

/**
    A VST3 instrument instance as the engine sees it (InstrumentProcessor).

    - prepare() runs on the message thread and holds `lock`; render() on the audio thread only
      *tries* the lock and outputs silence while the plugin is being (re)prepared, so the audio
      thread never waits.
    - Crash isolation level L1 (docs/05 §5): on Windows the plugin's processBlock is called
      inside SEH __try/__except, and C++ exceptions are caught everywhere. A plugin that faults
      is marked failed and stays silent; the rest of the show keeps playing. A fault that
      corrupted memory can still take the process down (that needs L2, a separate process).
*/
class PluginInstrument final : public InstrumentProcessor,
                               private juce::AudioProcessorListener
{
public:
    explicit PluginInstrument (std::unique_ptr<juce::AudioPluginInstance>);
    ~PluginInstrument() override;

    /** Message thread. Returns false (and an error) if the plugin refused the settings. */
    bool prepare (double sampleRate, int maxBlockSize, juce::String& error);
    bool isPreparedFor (double sampleRate, int maxBlockSize) const noexcept;

    bool render (float* outL, float* outR, int numSamples,
                 const MidiBytes* msgs, const int* offsets, int count) noexcept override;

    juce::AudioPluginInstance& getPlugin() noexcept { return *plugin; }
    bool hasFailed() const noexcept { return failed.load (std::memory_order_relaxed); }
    juce::String getFailure() const;

    /** Message thread: plugin state (getStateInformation), empty on failure. */
    juce::MemoryBlock getState();
    bool setState (const juce::MemoryBlock&);

    /** Average share of the block duration spent inside the plugin (0..1+), for the UI. */
    float getLoad() const noexcept { return load.load (std::memory_order_relaxed); }

    /** True once after the plugin reported a parameter / preset / state change (any thread),
        so the project can be marked unsaved. */
    bool takeChanged() noexcept { return changed.exchange (false); }

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::CriticalSection lock;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    double preparedRate = 0.0;
    int preparedBlock = 0;
    std::atomic<bool> failed { false };
    std::atomic<int> failureCode { 0 };
    std::atomic<float> load { 0.0f };
    std::atomic<bool> changed { false };

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { changed.store (true); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override
    {
        if (d.programChanged || d.nonParameterStateChanged || d.parameterInfoChanged)
            changed.store (true);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginInstrument)
};

/** Calls processBlock with SEH (Windows) and C++ exception protection. 0 = ok, else a fault code. */
int guardedProcess (juce::AudioProcessor&, juce::AudioBuffer<float>&, juce::MidiBuffer&) noexcept;

} // namespace b4nd
