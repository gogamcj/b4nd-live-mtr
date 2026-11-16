#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/Midi.h"
#include "core/OutputRouting.h"
#include "core/TempoMap.h"

namespace b4nd
{

/** Decoded audio at the engine sample rate, non-interleaved. Immutable once built. */
struct AudioData
{
    int numChannels = 0;
    std::int64_t numFrames = 0;
    std::vector<std::vector<float>> channels;

    const float* channel (int c) const noexcept
    {
        return channels.empty() ? nullptr : channels[(size_t) (c < numChannels ? c : numChannels - 1)].data();
    }
};

enum class TrackRole : std::uint8_t
{
    Audio,   // audio file(s) -> its bus (Main / Extra / Monitor / Aux)
    Click,   // generated metronome -> Click bus only
    Guide    // audio cue track -> Guide bus only
};

struct AudioClip
{
    std::shared_ptr<const AudioData> data;
    std::int64_t startSample = 0;   // song timeline position, engine sample rate
};

struct TrackRuntime
{
    int slot = 0;                          // index into the engine's track parameter slots
    TrackRole role = TrackRole::Audio;
    BusKind bus = BusKind::Main;           // forced to Click / Guide for those roles
    std::vector<AudioClip> clips;
};

/** A MIDI track that plays an external sound module (docs/04). */
struct MidiTrackRuntime
{
    int slot = 0;                          // engine track parameter slot (mute / solo)
    int port = 0;                          // MIDI output port index (see AudioEngine::maxMidiPorts)
    double sendEarlyMs = 0.0;              // module latency + track offset: positive = send earlier
    std::vector<MidiSampleEvent> events;   // sorted by sample
};

struct ClickSound
{
    std::vector<float> accent;   // first beat of the bar
    std::vector<float> normal;
};

/**
    Everything needed to play one song, built on a non-realtime thread and handed
    to the engine as a whole (docs/03 §5). The audio thread only reads it.
*/
struct PlaybackPlan
{
    std::int64_t lengthSamples = 0;
    TempoMap tempo;
    std::vector<TrackRuntime> tracks;
    std::vector<MidiTrackRuntime> midiTracks;
    bool hasClick = false;
    int clickSlot = -1;
    ClickSound click;

    /** A short synthetic click (sine burst with exponential decay). */
    static ClickSound makeDefaultClick (double sampleRate);
};

} // namespace b4nd
