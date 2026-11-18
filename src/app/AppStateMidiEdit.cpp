// MIDI editor saves (Phase 11). Kept apart from AppState.cpp, which is long enough.

#include "app/AppState.h"
#include "midi/MidiFileImport.h"

namespace b4nd
{

const SongInfo* AppState::findSong (const juce::String& songId) const noexcept
{
    for (const auto& s : songs)
        if (s.id == songId)
            return &s;
    return nullptr;
}

juce::String AppState::midiEditLockReason() const
{
    if (liveMode)
        return "Live Mode: MIDI cannot be edited during a show";
    if (transport == TransportState::Playing)
        return "Stop playback to save MIDI edits";
    return {};
}

juce::String AppState::saveMidiEdit (const juce::String& songId, const juce::String& trackId,
                                     const std::vector<MidiTickEvent>& events)
{
    if (auto g = midiEditLockReason(); g.isNotEmpty())
        return g;
    SongInfo* song = nullptr;
    int songIndex = -1;
    for (int i = 0; i < (int) songs.size(); ++i)
        if (songs[(size_t) i].id == songId)
        {
            song = &songs[(size_t) i];
            songIndex = i;
        }
    if (song == nullptr)
        return "The song is no longer in the setlist";
    TrackInfo* t = nullptr;
    for (auto& x : song->tracks)
        if (x.id == trackId)
            t = &x;
    if (t == nullptr)
        return "The track is no longer in the song";
    if (t->kind != TrackKind::MidiExternal && t->kind != TrackKind::MidiVsti)
        return "Not a MIDI track";

    int notes = 0;
    for (const auto& e : events)
        notes += e.msg.isNoteOn() ? 1 : 0;
    if (notes == 0)
        return "No notes left: nothing would play (mute or delete the track instead)";

    // A new file next to the project's other MIDI, named like the old one (AI parts are found by
    // the file name: bass.mid stays bass.mid). The previous file is never overwritten.
    const juce::File base = session.isOpen() ? session.getFolder().getChildFile ("midi")
                                             : getLastProjectRecordFile().getParentDirectory().getChildFile ("midi-edits");
    auto folder = base.getChildFile (song->id).getChildFile ("edit-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S"));
    for (int n = 2; folder.exists(); ++n)
        folder = folder.getSiblingFile (folder.getFileName().upToFirstOccurrenceOf ("_", false, false) + "_" + juce::String (n));
    const auto oldName = t->midiPath.isNotEmpty() ? juce::File (t->midiPath).getFileNameWithoutExtension()
                                                  : juce::File::createLegalFileName (t->name);
    const auto file = folder.getChildFile ((oldName.isNotEmpty() ? oldName : juce::String ("part")) + ".mid");
    if (auto err = writeMidiFile (file, events, song->tempo.bpmAtBeat (0.0), "B4ND edit " + t->name); err.isNotEmpty())
    {
        folder.deleteRecursively();
        return err;
    }

    const auto check = importMidiFile (file);
    if (! check.ok || check.noteCount != notes)
    {
        folder.deleteRecursively();
        return "The saved file did not read back correctly (" + (check.ok ? juce::String (check.noteCount) + " notes" : check.error)
               + "); the track was not changed";
    }

    const auto previous = t->midiPath;
    t->midiPath = file.getFullPathName();
    loadMidiFile (*t, *song);
    if (t->midiStatus != TrackInfo::MidiStatus::Ready)
    {
        const auto why = t->midiError;
        t->midiPath = previous;
        loadMidiFile (*t, *song);
        folder.deleteRecursively();
        return "Cannot use the saved file: " + why;
    }
    juce::Logger::writeToLog ("MIDI edit saved: " + t->name + " (" + juce::String (notes) + " notes) -> " + file.getFullPathName()
                              + (previous.isNotEmpty() ? " (previous: " + previous + ")" : juce::String()));
    markDirty();
    if (songIndex == currentSong)
        rebuildPlan();
    sendChangeMessage();
    return {};
}

} // namespace b4nd
