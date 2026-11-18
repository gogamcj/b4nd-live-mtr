#pragma once

#include "app/AppState.h"

namespace b4nd
{

/**
    Footswitch / MIDI control setup (docs/04 §8): which MIDI inputs to listen to, and which
    pedal does what (LEARN: press the pedal). Two presses for a binding ("2x") protect
    actions like EMERGENCY STOP from a stray step. Locked in Live Mode; the pedals work
    everywhere.
*/
class FootswitchPanel : public juce::Component,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    explicit FootswitchPanel (AppState&);
    ~FootswitchPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** LEARN on a row (tests / command line). */
    void learn (int row);
    int getLearningRow() const noexcept { return learningRow; }

private:
    class Row;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuildPorts();
    void rebuildRows();
    void pushBindings();
    void layoutRows();

    AppState& state;
    std::vector<FootswitchBinding> bindings;
    juce::OwnedArray<juce::ToggleButton> portToggles;
    std::vector<MidiPortInfo> portInfos;
    juce::OwnedArray<Row> rows;
    juce::Component rowHolder;
    juce::Viewport rowView;
    juce::TextButton rescanButton { "RESCAN" }, addButton { "+ ADD" }, defaultsButton { "DEFAULTS" };
    int learningRow = -1;
    int lastSeenCount = 0;
    juce::Rectangle<int> portsArea, portsHeader, rowsHeader, statusArea;

    friend class Row;
};

void showFootswitchWindow (AppState&);
FootswitchPanel* getFootswitchWindowContent();
void closeFootswitchWindow();

} // namespace b4nd
