#pragma once

#include "app/AppState.h"

namespace b4nd
{

/**
    Setlist manager (Phase 13): the show order, add / remove / move songs, title and artist,
    when each song starts in the show and the total time, and whether each song's audio is
    in RAM. Locked in Live Mode and while playing.
*/
class SetlistPanel : public juce::Component,
                     private juce::ChangeListener,
                     private juce::ListBoxModel,
                     private juce::Timer
{
public:
    explicit SetlistPanel (AppState&);
    ~SetlistPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int row) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void refresh();
    void report (const juce::String& error);

    AppState& state;
    juce::ListBox list { "setlist", this };
    juce::TextButton addButton { "+ ADD SONG" }, removeButton { "DELETE" }, upButton { "MOVE UP" }, downButton { "MOVE DOWN" },
                     applyButton { "RENAME" };
    juce::TextEditor titleEditor, artistEditor;
    juce::String message;
    bool messageIsError = false;
    juce::Rectangle<int> headerArea, columnsArea, editArea, summaryArea, messageArea;
};

void showSetlistWindow (AppState&);
SetlistPanel* getSetlistWindowContent();
void closeSetlistWindow();

} // namespace b4nd
