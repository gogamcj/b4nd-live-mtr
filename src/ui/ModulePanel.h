#pragma once

#include "app/AppState.h"

namespace b4nd
{

/**
    External MIDI sound modules (docs/04 §3): profiles (port, channel, bank/program,
    volume/pan, delay, send order), live status, test note and preset recall,
    and which module each external MIDI track of the current song plays on.
    Editing is blocked in Live Mode and while playing; TEST NOTE and RECALL always work.
*/
class ModulePanel : public juce::Component,
                    private juce::ChangeListener,
                    private juce::ListBoxModel,
                    private juce::Timer
{
public:
    explicit ModulePanel (AppState&);
    ~ModulePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Selects a module row (screenshots / tests). */
    void selectModule (int index);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    void loadEditor();
    void applyEditor();
    void refreshPorts();
    void rebuildTrackRows();
    void refreshEnablement();
    juce::String lockReason() const;

    AppState& state;
    juce::ListBox list { "modules", this };
    juce::TextButton addButton { "+ ADD MODULE" }, removeButton { "REMOVE" };

    juce::TextEditor nameEdit, msbEdit, lsbEdit, programEdit, volumeEdit, panEdit, delayEdit, gapEdit, settleEdit;
    juce::ComboBox portBox, channelBox, orderBox;
    juce::ToggleButton sustainToggle { "Sustain pedal (CC64)" };
    juce::TextButton applyButton { "APPLY" }, testButton { "TEST NOTE" }, recallButton { "SEND PRESET" },
                     refreshButton { "Rescan" };

    struct TrackRow
    {
        int trackIndex = 0;
        juce::String name;
        std::unique_ptr<juce::ComboBox> module, channel;
    };
    std::vector<TrackRow> trackRows;
    juce::String trackRowsFor;

    std::vector<MidiPortInfo> ports;
    int editing = -1;
    bool updating = false;
    juce::String message;
    bool messageIsError = false;

    juce::Rectangle<int> listHeader, editHeader, editArea, statusArea, tracksHeader, tracksArea, lockArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModulePanel)
};

void showModuleWindow (AppState&, int selectModule = -1);
void closeModuleWindow();
/** The panel of the open window (screenshots), or null. */
juce::Component* getModuleWindowContent();

} // namespace b4nd
