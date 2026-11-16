#pragma once

#include "app/AppState.h"
#include "ui/LevelMeter.h"

namespace b4nd
{

class ChannelStrip : public juce::Component
{
public:
    ChannelStrip (AppState&, int trackIndex);   // trackIndex < 0 = master

    void paint (juce::Graphics&) override;
    void resized() override;
    void syncFromState();
    void updateMeter() { meter.update(); }

private:
    AppState& state;
    const int track;
    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::Slider pan { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::TextButton muteButton { "M" }, soloButton { "S" };
    LevelMeter meter;
    juce::Rectangle<int> nameArea, routeArea;
};

/** Bottom area: channel strips for the current song, master, and per physical output meters. */
class MixerPanel : public juce::Component,
                   private juce::ChangeListener,
                   private juce::Timer
{
public:
    explicit MixerPanel (AppState&);
    ~MixerPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuildStrips();

    AppState& state;
    int builtForSong = -1;
    juce::OwnedArray<ChannelStrip> strips;
    std::unique_ptr<ChannelStrip> master;
    juce::OwnedArray<LevelMeter> outputMeters;
    int outputMeterWidth = 46;
    void rebuildOutputMeters();
    juce::Viewport stripViewport;
    juce::Component stripHolder;
    juce::Rectangle<int> outputsArea;
};

} // namespace b4nd
