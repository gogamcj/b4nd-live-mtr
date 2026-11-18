#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Midi.h"

namespace b4nd
{

/** What a footswitch (or any MIDI control) can do on stage (docs/04 §8). */
enum class LiveAction : std::uint8_t
{
    None,
    PlayPause,
    Stop,
    Restart,
    NextSong,
    PreviousSong,
    NextCue,
    PreviousCue,
    ToggleLoop,
    EmergencyStop,
    count
};

const char* liveActionId (LiveAction) noexcept;          // "play_pause" ... (saved in the DB)
const char* liveActionLabel (LiveAction) noexcept;       // "PLAY / PAUSE" ... (shown)
LiveAction liveActionFromId (const std::string&) noexcept;

struct FootswitchBinding
{
    std::string port;        // MIDI input name; empty = any port
    bool note = false;       // false: Control Change, true: Note On
    int channel = -1;        // 0-15, -1 = any channel
    int number = 64;         // CC or note number
    LiveAction action = LiveAction::None;
    bool doublePress = false;   // needs two presses within doublePressMs (against accidental presses)
    bool latching = false;      // CC pedal that sends 127 on one press and 0 on the next: every change is a press
};

/**
    Turns MIDI from footswitches into stage actions.

    - A press is a Note On, or a CC going from below 64 to 64 or above (pedals that send
      127 / 0, and expression pedals that send many values, both give one press). A pedal that
      sends the same value again (127, 127: "trigger" pedals) gives a press each time. Latching
      bindings take every change across 64, up or down, as a press.
    - Presses of one input closer than debounceMs are bounces and ignored.
    - A double-press binding fires on the second press within doublePressMs; the first press
      of such an input does nothing else (no single-press action on the same input).
    - Learn mode: the next press is reported instead of acted on.

    Pure logic with a fixed-size state; time comes from the caller. Runs on the MIDI input
    thread (never the audio thread). Call it from one thread only.
*/
class FootswitchMapper
{
public:
    static constexpr double debounceMs = 50.0;
    static constexpr double doublePressMs = 500.0;
    static constexpr int maxBindings = 32;

    static std::vector<FootswitchBinding> defaultBindings();

    /** Replace the bindings (not while handle() runs on another thread: the owner swaps whole mappers). */
    void setBindings (const std::vector<FootswitchBinding>&);
    const std::vector<FootswitchBinding>& getBindings() const noexcept { return bindings; }

    struct Result
    {
        LiveAction action = LiveAction::None;
        bool press = false;        // the message was a press of some input
        bool learned = false;      // learn mode: `input` is the pressed control
        bool waitingForSecond = false;   // first press of a double-press binding
        FootswitchBinding input;   // the pressed control (port / note / channel / number)
    };

    Result handle (const MidiBytes&, const std::string& port, double nowMs) noexcept;

    void setLearning (bool on) noexcept { learning = on; }
    bool isLearning() const noexcept { return learning; }
    void reset() noexcept;

private:
    int findBinding (bool note, int channel, int number, const std::string& port, bool wantDouble) const noexcept;

    std::vector<FootswitchBinding> bindings;
    bool learning = false;
    // per channel / number / kind: last press time, CC "down" state, time of a pending first press
    struct InputState { double lastPress = -1.0e9; double pending = -1.0e9; bool down = false; int lastValue = -1; };
    std::array<std::array<InputState, 128>, 32> inputs {};   // [kind * 16 + channel][number]
};

} // namespace b4nd
