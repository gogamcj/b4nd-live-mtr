#pragma once

#include "app/AppState.h"

namespace b4nd
{

/** Centre area: bar ruler, cue markers, Audio/MIDI track lanes and play head.
    Phase 1 shows the song fitted to the width; zoom/scroll arrives with the editor work. */
class TimelineView : public juce::Component,
                     private juce::ChangeListener
{
public:
    explicit TimelineView (AppState&);
    ~TimelineView() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::Rectangle<int> laneArea() const;
    float xForSeconds (double s) const;
    double secondsForX (float x) const;
    int trackHeight() const;

    static constexpr int headerWidth = 210;
    static constexpr int rulerHeight = 26;
    static constexpr int cueHeight = 22;

    AppState& state;
    float lastPlayheadX = -1.0f;
    bool draggingPlayhead = false;
};

} // namespace b4nd
