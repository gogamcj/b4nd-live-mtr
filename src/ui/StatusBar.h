#pragma once

#include "app/AppState.h"

namespace b4nd
{

/** Bottom status bar: audio device, MIDI, VSTi, CPU, memory, xruns, mode. */
class StatusBar : public juce::Component,
                  private juce::Timer
{
public:
    explicit StatusBar (AppState&);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override { repaint(); }
    AppState& state;
};

} // namespace b4nd
