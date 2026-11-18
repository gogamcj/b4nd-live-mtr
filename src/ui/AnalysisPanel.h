#pragma once

#include "app/AppState.h"

namespace b4nd
{

/**
    AI analysis of the current song (docs/07): runs the analyzer on an audio track, shows the
    draft result with confidences (below 70 % in yellow), lets the user correct it (BPM, half /
    double tempo, bar 1 position, time signature, key, section names) and applies the chosen
    parts to the song. Nothing changes the song until APPLY; one level of undo.
    Locked in Live Mode, and while playing.
*/
class AnalysisPanel : public juce::Component,
                      private juce::ChangeListener,
                      private juce::ListBoxModel,
                      private juce::Timer
{
public:
    explicit AnalysisPanel (AppState&);
    ~AnalysisPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** For tests and the command line: press APPLY with everything selected. */
    juce::String applyAll();

    /** Ticks "Stems" (once the analyzer check says Demucs is there): the Stem Separation button. */
    void preselectStems();
    /** Ticks "Audio to MIDI": the Audio to MIDI button. */
    void preselectMidi();

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    int getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int row, bool selected, juce::Component* existing) override;

    void refresh();
    void loadDraft (const AnalysisResult&);
    void fillSourceBox();
    void probeAnalyzer();
    void apply();
    double draftBeatSeconds() const;
    juce::String lockReason() const;
    void setMessage (const juce::String&, bool isError);

    AppState& state;

    juce::TextButton analyzerButton { "ANALYZER..." }, checkButton { "CHECK" }, analyzeButton { "ANALYZE" },
                     cancelButton { "CANCEL" }, applyButton { "APPLY TO SONG" }, undoButton { "UNDO APPLY" },
                     halfButton { "/2" }, doubleButton { "x2" }, earlierButton { "-1 BEAT" }, laterButton { "+1 BEAT" };
    juce::ComboBox sourceBox, timeSigBox, keyBox;
    juce::ToggleButton taskTempo { "Tempo / beats / bars" }, taskKey { "Key" }, taskChords { "Chords" },
                       taskSections { "Sections (cues)" }, taskStems { "Stems (Demucs, slow)" }, taskMidi { "Audio to MIDI (from stems)" };
    juce::ToggleButton useTempo { "TEMPO" }, useTimeSig { "TIME SIG" }, useKey { "KEY" }, useChords { "CHORDS" }, useCues { "CUES" }, useStems { "STEMS" }, useMidi { "MIDI" };
    juce::TextEditor bpmEditor, gridEditor;
    juce::ListBox sectionList { "sections", this };
    std::unique_ptr<juce::FileChooser> chooser;

    // The draft the user edits (from the last result).
    struct DraftSection { AnalysisResult::Section s; bool include = true; };
    std::vector<DraftSection> sections;
    juce::String draftFor;           // song id + result identity
    double tempoScale = 1.0;
    double gridSec = 0.0;

    juce::String analyzerInfo, analyzerError;
    bool analyzerChecked = false;
    bool stemsAvailable = false, stemsRequested = false, midiRequested = false, pitchedMidi = false;
    juce::String stemsReason;
    juce::String message;
    bool messageIsError = false;
    juce::Rectangle<int> headerArea, analyzerArea, progressArea, resultsArea, tempoInfoArea, barLabelArea, barInfoArea, sigInfoArea,
                         keyInfoArea, chordsArea, cuesInfoArea, stemsInfoArea, midiInfoArea, errorsArea, lockArea, messageArea, sectionsHeader;

    friend class SectionRow;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalysisPanel)
};

void showAnalysisWindow (AppState&);
void closeAnalysisWindow();
AnalysisPanel* getAnalysisWindowContent();

} // namespace b4nd
