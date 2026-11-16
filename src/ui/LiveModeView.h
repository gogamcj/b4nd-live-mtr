#pragma once

#include "app/AppState.h"
#include "ui/LevelMeter.h"

namespace b4nd
{

/** Full-screen performance view: very large text and buttons, stage-readable. */
class LiveModeView : public juce::Component,
                     private juce::ChangeListener,
                     private juce::Timer
{
public:
    explicit LiveModeView (AppState&);
    ~LiveModeView() override;

    std::function<void()> onExitClicked;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuildCueButtons();
    void paintBigCell (juce::Graphics&, juce::Rectangle<int>, const juce::String& caption, const juce::String& value,
                       juce::Colour) const;

    AppState& state;
    juce::TextButton prevButton { "<< PREV" }, restartButton { "RESTART" }, playButton { "PLAY" },
                     stopButton { "STOP" }, nextButton { "NEXT >>" }, loopButton { "LOOP" },
                     emergencyButton { "EMERGENCY\nSTOP" }, exitButton { "EXIT LIVE (F11)" };
    juce::OwnedArray<juce::TextButton> cueButtons;
    LevelMeter mainMeterL, mainMeterR, clickMeter;
    int cuesBuiltForSong = -1;

    juce::Rectangle<int> headerArea, cellsArea, progressArea, cuesArea, statusArea, metersArea;
};

} // namespace b4nd
