#pragma once

#include "app/AppState.h"
#include "core/TempoEdit.h"

namespace b4nd
{

/**
    Tempo / time signature / bar 1 / key of the current song, editable at any time (after AI
    analysis or without one): BPM with /2 and x2 and tap tempo, time signature (x/4 and x/8),
    bar 1 position (+-1 beat, +-10 ms, at the playhead) and key. Each change is applied at once,
    saved with the project, and can be undone (one level). Locked in Live Mode and while playing.
*/
class SongTimingPanel : public juce::Component,
                        private juce::ChangeListener
{
public:
    explicit SongTimingPanel (AppState&);
    ~SongTimingPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    /** For tests: presses the tap button at these times (seconds). */
    double tapAt (double seconds) { return tapped (tap.tap (seconds)); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void refresh();
    void run (const AppState::SongTimingEdit&, const juce::String& what);
    double tapped (double bpm);
    juce::String lockReason() const;

    AppState& state;
    TapTempo tap;
    juce::TextEditor bpmEditor, barEditor;
    juce::TextButton setBpm { "SET BPM" }, halfButton { "/2" }, doubleButton { "x2" }, tapButton { "TAP" },
                     setBar { "SET" }, beatEarlier { "-1 BEAT" }, beatLater { "+1 BEAT" }, nudgeEarlier { "-10 ms" },
                     nudgeLater { "+10 ms" }, atPlayhead { "AT PLAYHEAD" }, sig34 { "3/4" }, sig44 { "4/4" }, sig68 { "6/8" },
                     undoButton { "UNDO" }, analyseButton { "RE-ANALYSE..." };
    juce::ComboBox numBox, denBox, keyBox;
    juce::String message, shownFor;
    bool messageIsError = false;
    juce::Rectangle<int> infoArea, tempoLabel, sigLabel, barLabel, keyLabel, messageArea, warnArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongTimingPanel)
};

void showSongTimingWindow (AppState&);
SongTimingPanel* getSongTimingWindowContent();
void closeSongTimingWindow();

} // namespace b4nd
