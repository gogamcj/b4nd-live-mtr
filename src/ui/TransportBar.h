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
    /** A click on BPM / KEY / TIME SIG opens the tempo / time signature / key editor. */
    void mouseDown (const juce::MouseEvent&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh();
    void paintCell (juce::Graphics&, juce::Rectangle<int>, const juce::String& caption,
                    const juce::String& value, juce::Colour valueColour, bool monoValue) const;

    AppState& state;
    juce::TextButton restartButton { "|<" }, playButton { "PLAY" }, pauseButton { "PAUSE" },
                     stopButton { "STOP" }, loopButton { "LOOP" }, emergencyButton { "EMERGENCY STOP" },
                     liveButton { "LIVE MODE" }, clickButton { "CLICK" };
    void showClickMenu();

    juce::Rectangle<int> infoArea, cellsArea;
    juce::Rectangle<int> timingCells;   // BPM, KEY, TIME SIG (set in paint)
};

} // namespace b4nd
