#pragma once

#include "app/AppState.h"

namespace b4nd
{

/** Bottom status bar: audio device, SR/buffer/latency, MIDI, VSTi, CPU, dropouts, mode.
    Click to open the audio settings. */
class StatusBar : public juce::Component,
                  public juce::SettableTooltipClient,
                  private juce::Timer
{
public:
    explicit StatusBar (AppState&);
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    std::function<void()> onAudioSettingsRequested;

private:
    void timerCallback() override { repaint(); }
    AppState& state;
};

} // namespace b4nd
