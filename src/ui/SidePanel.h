#pragma once

#include "app/AppState.h"

namespace b4nd
{

/** Left panel: song list (project / setlist) and AI analysis menu. */
class SidePanel : public juce::Component,
                  private juce::ChangeListener,
                  private juce::ListBoxModel
{
public:
    explicit SidePanel (AppState&);
    ~SidePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;

    AppState& state;
    juce::ListBox songList { "songs", this };
    juce::OwnedArray<juce::TextButton> aiButtons;
    juce::TextButton newButton { "NEW" }, openButton { "OPEN" }, saveButton { "SAVE" };
    juce::TextButton modulesButton { "MIDI MODULES" };
    juce::Rectangle<int> songHeader, aiHeader, totalArea;
    bool lastLive = false;
    juce::String builtFor;
};

} // namespace b4nd
