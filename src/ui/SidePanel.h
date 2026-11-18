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
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TextButton newButton { "NEW" }, openButton { "OPEN" }, saveButton { "SAVE" };
    juce::TextButton modulesButton { "MIDI MODULES" }, pluginsButton { "PLUGINS" }, footswitchButton { "FOOTSWITCH" }, setlistButton { "EDIT SETLIST" }, showCheckButton { "SHOW CHECK" };
public:
    /** What the SHOW CHECK window's ENTER LIVE MODE does (set by the main window). */
    std::function<void()> onEnterLive;
private:
    juce::Rectangle<int> songHeader, aiHeader, totalArea;
    bool lastLive = false;
    juce::String builtFor;
};

} // namespace b4nd
