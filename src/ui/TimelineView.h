#pragma once

#include "app/AppState.h"

namespace b4nd
{

/** Centre area: bar ruler, cue markers, Audio/MIDI track lanes and play head.
    Phase 1 shows the song fitted to the width; zoom/scroll arrives with the editor work. */
class TimelineView : public juce::Component,
                     public juce::FileDragAndDropTarget,
                     private juce::ChangeListener
{
public:
    explicit TimelineView (AppState&);
    ~TimelineView() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    /** Double-click a MIDI lane: opens the MIDI editor on it. */
    void mouseDoubleClick (const juce::MouseEvent&) override;

    // Drop audio files on a lane to assign them, or below the lanes to add tracks.
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::Rectangle<int> laneArea() const;
    float xForSeconds (double s) const;
    double secondsForX (float x) const;
    int trackHeight() const;
    int trackAtY (int y) const;
    void showNotice (const juce::String& text, bool isError);

    static constexpr int headerWidth = 210;
    static constexpr int rulerHeight = 26;
    static constexpr int cueHeight = 22;
    /** Chord strip under the cues, only for songs with analysed chords. */
    int chordHeight() const { const auto* s = state.getCurrentSong(); return s != nullptr && ! s->chords.empty() ? 16 : 0; }
    int topHeight() const { return rulerHeight + cueHeight + chordHeight(); }

    AppState& state;
    float lastPlayheadX = -1.0f;
    bool draggingPlayhead = false;
    bool dropIsMidi = false;
    int dropTrack = -2;          // -2: no drag, -1: "add new track", >= 0: lane under the cursor
    juce::String notice;
    bool noticeIsError = false;
    std::uint32_t noticeTime = 0;
};

} // namespace b4nd
