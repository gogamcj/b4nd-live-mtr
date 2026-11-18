#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <functional>
#include <memory>
#include <vector>

#include "core/Footswitch.h"
#include "core/SpscQueue.h"
#include "midi/MidiOutputService.h"   // MidiPortInfo

namespace b4nd
{

/** The OS side of MIDI input, behind an interface for tests. Called on the message thread;
    the callback comes on a MIDI thread (one per port on some systems). */
class MidiInputBackend
{
public:
    using Callback = std::function<void (const MidiBytes&)>;
    virtual ~MidiInputBackend() = default;
    virtual std::vector<MidiPortInfo> listInputs() = 0;
    virtual bool open (const MidiPortInfo&, Callback) = 0;
    virtual void close (const juce::String& identifier) = 0;
};

std::unique_ptr<MidiInputBackend> createJuceMidiInputBackend();

/**
    Footswitch / MIDI control input (docs/04 §8).

    - Only the MIDI inputs the user switched on are opened (a keyboard's sustain pedal on
      another port can never start a song).
    - Every second the port list is checked: a pedal that is unplugged is shown as
      disconnected and reopened when it comes back (identifier first, then name).
    - The MIDI thread only runs the mapper and puts the result in a lock-free queue; the
      actions run on the message thread, through the same AppState calls as the buttons.
*/
class FootswitchInput : private juce::Timer,
                        private juce::AsyncUpdater
{
public:
    explicit FootswitchInput (std::unique_ptr<MidiInputBackend>);
    ~FootswitchInput() override;

    /** Starts the 1 s port check (not in tests that drive it by hand). */
    void start (bool withTimer = true);
    void shutdown();

    std::vector<MidiPortInfo> listInputs() { return backend->listInputs(); }

    struct PortState
    {
        MidiPortInfo port;
        enum class Status { Disconnected, OpenFailed, Open } status = Status::Disconnected;
    };
    /** The inputs to listen to. Opens new ones, closes removed ones. */
    void setPorts (const std::vector<MidiPortInfo>&);
    const std::vector<PortState>& getPorts() const noexcept { return ports; }

    void setBindings (const std::vector<FootswitchBinding>&);
    std::vector<FootswitchBinding> getBindings() const;

    /** Learn mode: the next press is given to onLearned instead of doing anything. */
    void setLearning (bool);
    bool isLearning() const;

    /** Message thread. */
    std::function<void (LiveAction)> onAction;
    std::function<void (const FootswitchBinding&)> onLearned;
    std::function<void()> onPortsChanged;

    /** Last press seen (for the status display), on the message thread. */
    struct LastPress { FootswitchBinding input; LiveAction action = LiveAction::None; bool waiting = false; double atMs = 0.0; int count = 0; };
    const LastPress& getLastPress() const noexcept { return last; }
    std::uint64_t getDroppedCount() const noexcept { return dropped.load(); }

    /** The MIDI-thread path, for tests and the command line: as if `msg` came in on port `portIndex`. */
    void receive (int portIndex, const MidiBytes& msg, double nowMs);
    /** Runs the queued actions now (tests; normally the async update does it). */
    void deliverPending() { handleAsyncUpdate(); }

private:
    void timerCallback() override;
    void handleAsyncUpdate() override;
    void checkPorts();
    void openPort (int index);

    struct Event
    {
        LiveAction action;
        bool learned, waiting, note;
        std::int8_t channel;
        std::uint8_t number;
        std::int16_t port;
        double atMs;
    };

    std::unique_ptr<MidiInputBackend> backend;
    std::vector<PortState> ports;
    std::vector<std::string> portNames;   // read on the MIDI thread under `lock`
    FootswitchMapper mapper;
    mutable juce::SpinLock lock;          // mapper + queue producer side (several MIDI threads)
    SpscQueue<Event, 256> queue;
    std::atomic<std::uint64_t> dropped { 0 };
    LastPress last;
};

} // namespace b4nd
