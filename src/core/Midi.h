#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/TempoMap.h"

namespace b4nd
{

/** Internal MIDI resolution (docs/04, docs/08): ticks per quarter note. */
constexpr int midiPpq = 960;

/** A short (channel voice) MIDI message: 1-3 bytes. No SysEx in the realtime path. */
struct MidiBytes
{
    std::uint8_t data[3] { 0, 0, 0 };
    std::uint8_t size = 0;

    static MidiBytes make (std::uint8_t a, std::uint8_t b, std::uint8_t c) noexcept { return { { a, b, c }, 3 }; }
    static MidiBytes make (std::uint8_t a, std::uint8_t b) noexcept { return { { a, b, 0 }, 2 }; }

    std::uint8_t status() const noexcept   { return (std::uint8_t) (data[0] & 0xf0); }
    int channel() const noexcept           { return data[0] & 0x0f; }
    bool isNoteOn() const noexcept         { return status() == 0x90 && data[2] > 0; }
    bool isNoteOff() const noexcept        { return status() == 0x80 || (status() == 0x90 && data[2] == 0); }
    bool isController (int cc) const noexcept { return status() == 0xb0 && data[1] == cc; }
};

/** Expected length of a channel message from its status byte (0 = not a channel message). */
int midiMessageLength (std::uint8_t status) noexcept;

struct MidiTickEvent
{
    std::int64_t tick = 0;   // at midiPpq
    MidiBytes msg;
};

struct MidiSampleEvent
{
    std::int64_t sample = 0;   // song timeline, engine sample rate
    MidiBytes msg;
};

/**
    Converts tick events to sample positions with the song's tempo map.
    Output is sorted by sample; at the same sample, note-offs come before note-ons
    (a repeated note never gets cut by its own previous note-off).
    `forceChannel` 0-15 rewrites the channel of every message, -1 keeps it.
*/
std::vector<MidiSampleEvent> toSampleEvents (const std::vector<MidiTickEvent>& events, const TempoMap& tempo,
                                             double sampleRate, int forceChannel = -1);

/**
    Which notes are sounding on one MIDI port, plus sustain pedal state.
    Fixed size, no allocation: safe on the audio thread (docs/04 §6).
*/
class ActiveNoteTracker
{
public:
    void reset() noexcept;
    void observe (const MidiBytes&) noexcept;
    bool isNoteOn (int channel, int note) const noexcept;
    bool anyActive() const noexcept;
    int activeCount() const noexcept;

    /** Emits Note Off for every sounding note, then CC64 = 0 and CC123 (All Notes Off)
        on every channel that was used, and clears the state. `send (const MidiBytes&)`. */
    template <typename Send>
    void releaseAll (Send&& send) noexcept
    {
        for (int ch = 0; ch < 16; ++ch)
        {
            if (! usedChannels[(size_t) ch])
                continue;
            for (int n = 0; n < 128; ++n)
                if (notes[(size_t) ch][(size_t) n])
                    send (MidiBytes::make ((std::uint8_t) (0x80 | ch), (std::uint8_t) n, 0));
            send (MidiBytes::make ((std::uint8_t) (0xb0 | ch), 64, 0));
            send (MidiBytes::make ((std::uint8_t) (0xb0 | ch), 123, 0));
        }
        reset();
    }

private:
    std::array<std::array<bool, 128>, 16> notes {};
    std::array<bool, 16> sustain {};
    std::array<bool, 16> usedChannels {};
};

/** External sound module profile (docs/04 §3, docs/08 module_profile). */
struct ModuleProfile
{
    std::string id;
    std::string deviceName;          // shown to the user, e.g. "Roland INTEGRA-7"
    std::string portName;            // MIDI output port name
    std::string portIdentifier;      // OS identifier when known (more stable than the name)
    int channel = 1;                 // 1-16
    int bankMsb = -1, bankLsb = -1;  // -1 = do not send
    int program = -1;                // 0-127, -1 = do not send
    int volumeCc = 7, volume = 100;  // volume -1 = do not send
    int panCc = 10, pan = 64;        // pan -1 = do not send
    bool sustainEnabled = true;
    double delayOffsetMs = 0.0;      // module latency compensation (positive = module is slow, send earlier)
    std::string sendOrder = "msb,lsb,pc";
    int gapMs = 20;                  // between bank / program messages
    int settleMs = 200;              // after the last message before "Ready"
};

struct TimedMidi
{
    double atMs = 0.0;   // relative to the start of the recall
    MidiBytes msg;
};

/** The preset recall sequence for a profile and the time when the module is ready. */
std::vector<TimedMidi> presetRecallSequence (const ModuleProfile&, double* readyAtMs = nullptr);

} // namespace b4nd
