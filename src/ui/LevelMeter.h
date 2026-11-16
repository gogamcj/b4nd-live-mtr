#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

namespace b4nd
{

/** Vertical peak meter with hold. Reads its level from a provider (UI thread, ~30 Hz).
    From Phase 3 the provider reads audio-engine telemetry atomics. */
class LevelMeter : public juce::Component
{
public:
    using Provider = std::function<float()>;   // linear peak 0..1(+)

    explicit LevelMeter (Provider p = {}) : provider (std::move (p)) {}

    void setProvider (Provider p) { provider = std::move (p); }
    void update();   // call from a UI timer
    void paint (juce::Graphics&) override;

    static float toMeterProportion (float linear) noexcept;   // -60..+6 dB mapped to 0..1

private:
    Provider provider;
    float level = 0.0f;
    float hold = 0.0f;
    int holdFrames = 0;
    bool clipped = false;
};

} // namespace b4nd
