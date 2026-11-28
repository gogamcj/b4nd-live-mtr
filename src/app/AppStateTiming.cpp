// AppState: hand corrections of a song's tempo map, time signature, bar 1 and key (after AI
// analysis, or for a song without one). Message thread only; the engine gets the new map
// through rebuildPlan() like any other song change, and never while playing.

#include "app/AppState.h"

#include "core/TempoEdit.h"

#include <cmath>

namespace b4nd
{

void AppState::afterTempoChange (SongInfo& s)
{
    // MIDI regions are drawn in seconds from the tempo map: work them out again.
    for (auto& t : s.tracks)
        if (t.midiPath.isNotEmpty() && t.midiStatus == TrackInfo::MidiStatus::Ready)
            loadMidiFile (t, s);
}

juce::String AppState::editSongTiming (const SongTimingEdit& e)
{
    if (liveMode)
        return "Live Mode: songs cannot be changed during a show";
    if (transport != TransportState::Stopped)
        return "Stop playback first";
    auto* s = currentSongMutable();
    if (s == nullptr)
        return "No song";

    TempoMap m = s->tempo, next;
    juce::StringArray done;
    if (std::abs (e.tempoScale - 1.0) > 1e-9)
    {
        if (! tempoEdit::scale (m, e.tempoScale, next))
            return "Cannot " + juce::String (e.tempoScale > 1.0 ? "double" : "halve") + " the tempo (20 - 400 BPM)";
        m = next;
        done.add (e.tempoScale > 1.0 ? "tempo x2" : "tempo /2");
    }
    if (e.bpm > 0.0)
    {
        if (! tempoEdit::constantTempo (m, e.bpm, next))
            return "BPM must be 20 - 400";
        m = next;
        done.add (juce::String (e.bpm, 2) + " BPM");
    }
    if (e.numerator > 0)
    {
        const int d = e.denominator > 0 ? e.denominator : 4;
        if (! tempoEdit::timeSignature (m, e.numerator, d, next))
            return "Time signature must be 1-16 / 2, 4, 8 or 16";
        m = next;
        done.add (juce::String (e.numerator) + "/" + juce::String (d));
    }
    if (e.barOneSec >= 0.0)
    {
        if (! tempoEdit::barOneAt (m, e.barOneSec, next))
            return "Invalid bar 1 position";
        m = next;
    }
    if (std::abs (e.shiftBeats) > 1e-9)
    {
        if (! tempoEdit::shiftBarOne (m, e.shiftBeats, next))
            return "Invalid bar 1 shift";
        m = next;
    }
    if (e.barOneSec >= 0.0 || std::abs (e.shiftBeats) > 1e-9)
        done.add ("bar 1 at " + juce::String (m.getOffsetSeconds(), 3) + " s");
    const juce::String key = e.key.trim().substring (0, 40);
    if (key.isNotEmpty() && key != s->key)
        done.add (key);
    if (done.isEmpty())
        return "Nothing changed";

    TimingUndo u { s->id, s->tempo, s->key, s->bpmConfidence, s->keyConfidence };
    const bool tempoChanged = std::abs (e.tempoScale - 1.0) > 1e-9 || e.bpm > 0.0;
    s->tempo = m;
    if (tempoChanged)
        s->bpmConfidence = -1.0;   // set by hand: no AI confidence any more
    if (key.isNotEmpty() && key != s->key)
    {
        s->key = key;
        s->keyConfidence = -1.0;
    }
    timingUndo = std::move (u);
    afterTempoChange (*s);
    rebuildPlan();
    markDirty();
    juce::Logger::writeToLog ("Song timing edited (" + s->title + "): " + done.joinIntoString (", "));
    sendChangeMessage();
    return {};
}

bool AppState::canUndoSongTiming() const
{
    const auto* s = getCurrentSong();
    return timingUndo.has_value() && s != nullptr && s->id == timingUndo->songId;
}

juce::String AppState::undoSongTiming()
{
    if (liveMode)
        return "Live Mode: songs cannot be changed during a show";
    if (transport != TransportState::Stopped)
        return "Stop playback first";
    if (! canUndoSongTiming())
        return "Nothing to undo";
    auto* s = currentSongMutable();
    s->tempo = timingUndo->tempo;
    s->key = timingUndo->key;
    s->bpmConfidence = timingUndo->bpmConfidence;
    s->keyConfidence = timingUndo->keyConfidence;
    timingUndo.reset();
    afterTempoChange (*s);
    rebuildPlan();
    markDirty();
    juce::Logger::writeToLog ("Song timing edit undone: " + s->title);
    sendChangeMessage();
    return {};
}

juce::String AppState::songTimingWarning() const
{
    const auto* s = getCurrentSong();
    if (s == nullptr)
        return {};
    juce::StringArray parts;
    for (const auto& t : s->tracks)
        if (t.aiPart.isNotEmpty())
            parts.add (t.name);
    if (parts.isEmpty())
        return {};
    return "AI MIDI (" + parts.joinIntoString (", ") + ") was written at the old tempo: after a tempo or bar 1 change, "
           "run Audio to MIDI again so that it lines up with the audio.";
}

} // namespace b4nd
