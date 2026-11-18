#include "midi/MidiFileImport.h"

#include <algorithm>

namespace b4nd
{

bool isMidiFile (const juce::File& f)
{
    return f.hasFileExtension ("mid;midi;smf");
}

MidiImport importMidiData (const void* data, size_t size)
{
    MidiImport r;
    juce::MemoryInputStream in (data, size, false);
    juce::MidiFile mf;
    int smfType = 0;
    if (! mf.readFrom (in, true, &smfType))
    {
        r.error = "Not a readable MIDI file";
        return r;
    }
    const int tpq = mf.getTimeFormat();
    if (tpq <= 0)
    {
        r.error = "SMPTE-timed MIDI files are not supported (export with bars/beats timing)";
        return r;
    }

    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        const auto* seq = mf.getTrack (t);
        for (const auto* h : *seq)
        {
            const auto& m = h->message;
            if (m.isTempoMetaEvent() && r.fileBpm <= 0.0)
                r.fileBpm = 60.0 / m.getTempoSecondsPerQuarterNote();
            const auto* raw = m.getRawData();
            const int len = m.getRawDataSize();
            if (len < 1 || raw[0] < 0x80 || raw[0] >= 0xf0)
                continue;
            const int need = midiMessageLength (raw[0]);
            if (need == 0 || len < need)
                continue;
            MidiTickEvent e;
            e.tick = (std::int64_t) std::llround (m.getTimeStamp() * (double) midiPpq / (double) tpq);
            e.msg = need == 3 ? MidiBytes::make (raw[0], (std::uint8_t) (raw[1] & 0x7f), (std::uint8_t) (raw[2] & 0x7f))
                              : MidiBytes::make (raw[0], (std::uint8_t) (raw[1] & 0x7f));
            if (e.msg.isNoteOn())
                ++r.noteCount;
            r.channelsUsed |= (std::uint16_t) (1u << e.msg.channel());
            r.lastTick = std::max (r.lastTick, e.tick);
            r.events.push_back (e);
        }
    }
    std::stable_sort (r.events.begin(), r.events.end(), [] (const MidiTickEvent& a, const MidiTickEvent& b) { return a.tick < b.tick; });
    if (r.events.empty())
    {
        r.error = "The MIDI file has no notes or controllers";
        return r;
    }
    r.ok = true;
    return r;
}

juce::String writeMidiFile (const juce::File& file, const std::vector<MidiTickEvent>& events, double bpm,
                            const juce::String& trackName)
{
    juce::MidiMessageSequence meta, seq;
    if (trackName.isNotEmpty())
        meta.addEvent (juce::MidiMessage::textMetaEvent (3, trackName), 0.0);
    meta.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60'000'000.0 / juce::jlimit (10.0, 400.0, bpm > 0.0 ? bpm : 120.0))), 0.0);
    for (const auto& e : events)
    {
        const int len = midiMessageLength (e.msg.data[0]);
        if (len == 0 || e.msg.size < len)
            continue;
        seq.addEvent (juce::MidiMessage (e.msg.data, len, (double) e.tick));   // keeps the given order at equal ticks
    }
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (midiPpq);
    mf.addTrack (meta);
    mf.addTrack (seq);

    if (file.isDirectory())
        return "Cannot write " + file.getFullPathName() + ": a folder has that name";
    if (! file.getParentDirectory().createDirectory())
        return "Cannot create the folder " + file.getParentDirectory().getFullPathName();
    const auto tmp = file.getSiblingFile (file.getFileName() + ".tmp");
    tmp.deleteFile();   // a leftover from a crash: FileOutputStream would append to it
    {
        juce::FileOutputStream out (tmp);
        if (! out.openedOk() || ! mf.writeTo (out, 1))
        {
            tmp.deleteFile();
            return "Cannot write " + file.getFullPathName();
        }
        out.flush();
        if (out.getStatus().failed())
        {
            tmp.deleteFile();
            return "Cannot write " + file.getFullPathName() + ": " + out.getStatus().getErrorMessage();
        }
    }
    if (! tmp.moveFileTo (file))
    {
        tmp.deleteFile();
        return "Cannot replace " + file.getFullPathName();
    }
    return {};
}

MidiImport importMidiFile (const juce::File& f)
{
    if (! f.existsAsFile())
    {
        MidiImport r;
        r.error = "File not found";
        return r;
    }
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb))
    {
        MidiImport r;
        r.error = "Cannot read the file";
        return r;
    }
    return importMidiData (mb.getData(), mb.getSize());
}

} // namespace b4nd
