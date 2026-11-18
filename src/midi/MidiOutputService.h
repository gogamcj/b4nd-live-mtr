#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "core/Midi.h"
#include "engine/AudioEngine.h"

namespace b4nd
{

struct MidiPortInfo
{
    juce::String name;
    juce::String identifier;
};

/**
    The OS side of MIDI output, behind an interface so the scheduler can be
    tested without hardware. Slots are the engine's MIDI port numbers
    (one per module profile).

    listOutputs / open / close are called on the message thread,
    send on the MIDI output thread. The service serialises open/close/send per slot.
*/
class MidiPortBackend
{
public:
    virtual ~MidiPortBackend() = default;
    virtual std::vector<MidiPortInfo> listOutputs() = 0;
    virtual bool open (int slot, const MidiPortInfo&) = 0;
    virtual void close (int slot) = 0;
    virtual void send (int slot, const MidiBytes&) = 0;
};

/** juce::MidiOutput (WinMM / WinRT on Windows, ALSA on Linux). */
std::unique_ptr<MidiPortBackend> createJuceMidiBackend();

/**
    External MIDI output (docs/04 §3-§6).

    - A high priority thread takes the engine's MidiOutEvents and sends each one
      when its due time (steady clock) comes: sleep while far away, then spin.
    - Preset recall, test notes and panic go through the same thread, so the
      order on the wire is always well defined.
    - Every second the port list is checked. A port that disappears is marked
      Disconnected; when the same port comes back (identifier first, then name)
      it is reopened, all notes are reset and the module's preset is sent again.
    - panic() sends Note Off for everything this service has sent, then
      CC64 0 + CC123 on every used channel: used when the audio device is lost
      (the engine can no longer release its notes) and on emergency stop.
*/
class MidiOutputService : private juce::Thread,
                          private juce::Timer
{
public:
    enum class PortStatus
    {
        NoPort,        // the profile has no port selected
        Disconnected,  // the port is not present
        OpenFailed,    // present but cannot be opened (used by another program?)
        Preparing,     // preset recall sent, waiting for the settle time
        Ready
    };

    /** `engine` may be null (tests). The backend is owned. */
    MidiOutputService (AudioEngine* engine, std::unique_ptr<MidiPortBackend> backend);
    ~MidiOutputService() override;

    /** Starts the output thread and (withTimer) the port check / status timer on the message thread. */
    void start (bool withTimer = true);
    void shutdown();

    /** Module i uses slot i (= MidiTrackRuntime::port). Opens new ports, closes removed
        ones and sends the preset again when the port or the preset changed. */
    void setModules (const std::vector<ModuleProfile>&);

    PortStatus getStatus (int slot) const noexcept;
    static juce::String statusText (PortStatus);

    /** Sends the module's preset recall again (status goes to Preparing, then Ready). */
    void recallPreset (int slot);
    /** Middle C on the module's channel for `lengthMs`. */
    void sendTestNote (int slot, int note = 60, int velocity = 100, int lengthMs = 400);
    /** Note Off for every sounding note on every port, then CC64 0 + CC123. Pending engine events are dropped. */
    void panic();

    /** Queues one message on a slot `delayMs` from now (message thread). */
    /** minGapMs: never sent sooner than this after the previous message on the slot, even if
        the MIDI thread woke late (bank / program gaps that old modules need). */
    void sendLater (int slot, const MidiBytes&, double delayMs, double minGapMs = 0.0);

    /** Checks the port list now (normally once a second). */
    void pollPorts();
    /** Preparing -> Ready once the settle time has passed (normally every 100 ms). */
    void updatePreparing();

    std::vector<MidiPortInfo> listOutputs() { return backend->listOutputs(); }

    // telemetry (any thread)
    std::uint64_t getSentCount() const noexcept     { return sent.load (std::memory_order_relaxed); }
    std::uint64_t getDroppedCount() const noexcept  { return dropped.load (std::memory_order_relaxed); }
    /** Messages sent more than 2 ms after their due time. */
    std::uint64_t getLateCount() const noexcept     { return late.load (std::memory_order_relaxed); }
    double getMaxLatenessMs() const noexcept        { return (double) maxLateNs.load (std::memory_order_relaxed) / 1.0e6; }
    void resetTimingStats() noexcept                { late = 0; maxLateNs = 0; }

    /** Called on the message thread when a port status changes. */
    std::function<void()> onStatusChanged;

    static constexpr int maxSlots = AudioEngine::maxMidiPorts;

private:
    struct Pending
    {
        std::int64_t dueNs = 0;
        std::uint64_t seq = 0;   // keeps the order of events due at the same time
        int slot = 0;
        MidiBytes msg;
        std::int64_t minGapNs = 0;   // after the previous message sent on this slot
        bool done = false;           // MIDI thread scratch
    };

    struct Slot
    {
        ModuleProfile profile;
        bool assigned = false;
        bool open = false;
        std::atomic<int> status { (int) PortStatus::NoPort };
        std::atomic<int> channel { 0 };   // 0-15, read by the MIDI thread
        double readyAtMs = 0.0;
        MidiPortInfo openedAs;
    };

    void run() override;
    void timerCallback() override;
    void setStatus (int slot, PortStatus);
    void openSlot (int slot, const MidiPortInfo&);
    void closeSlot (int slot);
    void queueRecall (int slot);
    void sendOnThread (const Pending&);
    void doPanicOnThread();

    AudioEngine* engine;
    std::unique_ptr<MidiPortBackend> backend;
    std::array<Slot, maxSlots> slots;
    std::array<juce::CriticalSection, maxSlots> slotLocks;   // open/close vs send

    juce::CriticalSection immediateLock;
    std::vector<Pending> immediate;   // from the message thread

    // MIDI thread only
    std::vector<Pending> pending;
    std::array<std::int64_t, maxSlots> lastSentNs {};   // MIDI thread only
    std::vector<Pending> incoming;
    std::uint64_t seqCounter = 0;
    std::array<ActiveNoteTracker, maxSlots> sounding;
    std::array<std::atomic<bool>, maxSlots> resetSounding {};

    std::atomic<bool> panicRequested { false };
    std::atomic<std::uint64_t> sent { 0 }, dropped { 0 }, late { 0 };
    std::atomic<std::int64_t> maxLateNs { 0 };
    int pollCountdown = 0;
    juce::String lastPortList { "-" };
    bool timerPeriodRaised = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiOutputService)
};

} // namespace b4nd
