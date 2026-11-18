#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

#include "core/Midi.h"

namespace b4nd
{

/**
    Standard MIDI File import (format 0 and 1). All tracks are merged; only
    channel voice messages are kept (SysEx and meta events are not sent live).
    Ticks are rescaled to midiPpq (960). The file's beats are placed on the
    song's tempo map: bar 1 beat 1 of the file = the start of the song.
*/
struct MidiImport
{
    bool ok = false;
    juce::String error;
    std::vector<MidiTickEvent> events;   // sorted by tick
    int noteCount = 0;
    std::uint16_t channelsUsed = 0;      // bit n = channel n+1
    double fileBpm = 0.0;                // first tempo in the file (information only)
    std::int64_t lastTick = 0;
};

MidiImport importMidiFile (const juce::File&);
MidiImport importMidiData (const void* data, size_t size);

bool isMidiFile (const juce::File&);

/**
    Writes tick events (midiPpq) as a format 1 Standard MIDI File: a tempo track (the song's
    first tempo, so other programs open it at the right speed) and one track with the events.
    Written to a temporary file first and then moved into place: a failed write never leaves a
    half-written file. Returns an empty string, or the reason.
*/
juce::String writeMidiFile (const juce::File&, const std::vector<MidiTickEvent>& events, double bpm,
                            const juce::String& trackName);

} // namespace b4nd
