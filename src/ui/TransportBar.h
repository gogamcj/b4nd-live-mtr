#pragma once

#include "app/AppState.h"

namespace b4nd
{

/** Top bar: project, song, BPM, key, time signature, position and transport controls. */
class TransportBar : public juce::Component,
                     private juce::ChangeListener
{
public:
    explicit TransportBar (AppState&);
    ~TransportBar() override;

    std::function<void()> onLiveModeClicked;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh();
    void paintCell (juce::Graphics&, juce::Rectangle<int>, const juce::String& caption,
                    const juce::String& value, juce::Colour valueColour, bool monoValue) const;

    AppState& state;
    juce::TextButton restartButton { "|<" }, playButton { "PLAY" }, pauseButton { "PAUSE" },
                     stopButton { "STOP" }, loopButton { "LOOP" }, emergencyButton { "EMERGENCY STOP" },
                     liveButton { "LIVE MODE" };

    juce::Rectangle<int> infoArea, cellsArea;
};

} // namespace b4nd
