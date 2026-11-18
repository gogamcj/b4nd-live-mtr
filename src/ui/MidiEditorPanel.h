#pragma once

#include "app/AppState.h"
#include "core/MidiNoteEdit.h"

namespace b4nd
{

/**
    MIDI editor (Phase 11): piano roll of one MIDI track (a dropped .mid file or a part converted
    by AI), drawn on the song's bars and beats with an audio track's waveform behind the notes so
    that the notes can be lined up with what the band hears.

    Edits change a draft only. SAVE writes a new .mid file and points the track at it (the old
    file stays on disk); saving is locked while playing and the editor closes in Live Mode.
    Undo / redo go back through every edit, also past a save.
*/
class MidiEditorPanel : public juce::Component,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    explicit MidiEditorPanel (AppState&);
    ~MidiEditorPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Opens a MIDI track of a song. Empty string on success, else why not. */
    juce::String openTrack (const juce::String& songId, const juce::String& trackId);
    const juce::String& getSongId() const noexcept { return songId; }
    const juce::String& getTrackId() const noexcept { return trackId; }
    juce::String getTrackName() const;

    // Editing (the buttons, keys and the command line use these).
    const NoteSequence& getSequence() const noexcept { return seq; }
    const NoteSelection& getSelection() const noexcept { return selection; }
    void selectAll();
    void selectNone();
    void quantizeSelection();
    void transposeSelection (int semitones);
    void moveSelection (std::int64_t ticks);
    void changeVelocity (int delta);
    int deleteSelection();
    void undo();
    void redo();
    void revert();
    juce::String save();
    bool hasUnsavedEdits() const { return history.isModified(); }
    /** Grid in ticks (also the length of new notes). */
    std::int64_t getGridTicks() const noexcept { return gridTicks; }
    void setGridTicks (std::int64_t);
    /** Audio track whose waveform is drawn behind the notes; empty: none. */
    void setBackgroundTrack (const juce::String& id);
    const juce::String& getBackgroundTrack() const noexcept { return backgroundId; }

    /** Applies an edit to the draft: a step in the undo history. */
    void commit (const NoteSequence& edited, const NoteSelection& newSelection);

private:
    class Roll;
    friend class Roll;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshControls();
    void fillBackgroundBox();
    void setMessage (const juce::String&, bool isError);
    const SongInfo* song() const;
    const TrackInfo* track() const;

    AppState& state;
    juce::String songId, trackId, loadedPath, backgroundId;
    NoteSequence seq;
    NoteSelection selection;
    NoteEditHistory history;
    std::int64_t gridTicks = midiPpq / 4;
    bool snap = true;
    bool fileChangedUnderneath = false;
    bool wasLocked = false;

    std::unique_ptr<Roll> roll;
    juce::Label titleLabel;
    juce::ComboBox gridBox, backgroundBox;
    juce::ToggleButton snapButton { "SNAP" };
    juce::TextButton quantizeButton { "QUANTIZE" }, velDownButton { "VEL -" }, velUpButton { "VEL +" },
                     octDownButton { "-12" }, downButton { "-1" }, upButton { "+1" }, octUpButton { "+12" },
                     deleteButton { "DELETE" }, undoButton { "UNDO" }, redoButton { "REDO" },
                     zoomOutButton { "-" }, zoomInButton { "+" }, fitButton { "FIT" },
                     revertButton { "REVERT" }, saveButton { "SAVE" };
    juce::String message;
    bool messageIsError = false;
    juce::Rectangle<int> statusArea;
};

/** Opens the editor window on a track of the current song (asks before dropping unsaved edits
    of another track). Not in Live Mode. */
void showMidiEditor (AppState&, int trackIndex);
MidiEditorPanel* getMidiEditorContent();
void closeMidiEditor();

} // namespace b4nd
