#pragma once

#include "app/AppState.h"

namespace b4nd
{

/**
    Pre-show check (docs/10 §5): audio device, dropouts, output test, audio files, RAM, MIDI
    modules, VSTi, footswitch, project saved, Windows power plan. Red items stop Live Mode
    unless the user deliberately enters anyway (rehearsal); yellow items are warnings.
*/
class ShowCheckPanel : public juce::Component,
                       private juce::Timer
{
public:
    ShowCheckPanel (AppState&, std::function<void()> enterLive);

    void paint (juce::Graphics&) override;
    void resized() override;
    void recheck();
    const std::vector<AppState::ShowCheckItem>& getItems() const noexcept { return items; }

private:
    void timerCallback() override { recheck(); }

    AppState& state;
    std::function<void()> enterLive;
    std::vector<AppState::ShowCheckItem> items;
    juce::TextButton recheckButton { "CHECK AGAIN" }, audioButton { "AUDIO / OUTPUT TEST..." }, liveButton { "ENTER LIVE MODE" };
    juce::Rectangle<int> listArea;
};

/** Opens the check. `enterLive` (may be empty) is what ENTER LIVE MODE does. */
void showShowCheckWindow (AppState&, std::function<void()> enterLive);
ShowCheckPanel* getShowCheckWindowContent();
void closeShowCheckWindow();

} // namespace b4nd
