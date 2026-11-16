#pragma once

#include "app/AppState.h"

namespace b4nd
{

/**
    VST3 plugins (docs/05): the scanned plugin list with Live Certified / Not Certified /
    Blacklisted status, scanning (each plugin file in a separate scanner process), extra plugin
    folders, and which plugin each VSTi track of the current song plays, with its editor.
    Changes are blocked in Live Mode and while playing.
*/
class PluginPanel : public juce::Component,
                    private juce::ChangeListener,
                    private juce::ListBoxModel,
                    private juce::Timer
{
public:
    explicit PluginPanel (AppState&);
    ~PluginPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override { repaint(); }

    void rebuildTrackRows();
    void refreshEnablement();
    void startScan (bool onlyChanged);
    juce::String lockReason() const;
    void setMessage (const juce::String& text, bool isError);

    AppState& state;
    juce::ListBox list { "plugins", this };
    juce::TextButton scanButton { "SCAN NEW / CHANGED" }, rescanButton { "RESCAN ALL" }, folderButton { "+ FOLDER" },
                     clearButton { "CLEAR BLACKLIST" }, addTrackButton { "+ ADD VSTi TRACK WITH SELECTED" };
    std::unique_ptr<juce::FileChooser> chooser;

    struct TrackRow
    {
        int trackIndex = 0;
        std::unique_ptr<juce::ComboBox> plugin;
        std::unique_ptr<juce::TextButton> edit;
    };
    std::vector<TrackRow> trackRows;
    juce::String trackRowsFor;
    std::vector<juce::String> choiceUids;   // combo item id - 2 -> uid

    juce::String message;
    bool messageIsError = false;
    juce::Rectangle<int> listHeader, detailArea, progressArea, tracksHeader, tracksArea, lockArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPanel)
};

void showPluginWindow (AppState&);
void closePluginWindow();
juce::Component* getPluginWindowContent();

} // namespace b4nd
