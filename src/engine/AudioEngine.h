#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <memory>
#include <vector>

#include "core/SampleClock.h"
#include "core/OutputRouting.h"
#include "core/SpscQueue.h"
#include "core/XrunDetector.h"
#include "engine/PlaybackPlan.h"

namespace b4nd
{

enum class EngineTransport : int
{
    Stopped,
    Playing,
    Paused
};

enum class TestSignal : int
{
    None,
    Tone,        // 1 kHz sine on one output
    Identify,    // N beeps on output N (1-based), repeating: tells you which cable is which
    BusCheck,    // click pattern on every Click output, noise bursts on every Guide output, PA silent
    WalkOutputs  // 1 kHz tone stepping through all outputs, 1.5 s each
};

struct EngineCommand
{
    enum class Type : std::uint8_t
    {
        Play,
        Pause,
        Stop,
        Locate,          // samples
        EmergencyStop,
        SetSongLength,   // samples (0 = unlimited)
        StartTest,       // intValue = TestSignal, output = target output, value = level dB
        StopTest
    };

    Type type = Type::Stop;
    std::int64_t samples = 0;
    std::int32_t intValue = 0;
    std::int32_t output = -1;
    float value = 0.0f;
};

/** One MIDI message for the MIDI output thread, with the wall-clock time it should leave. */
struct MidiOutEvent
{
    std::int64_t dueNs = 0;   // steady_clock nanoseconds (see AudioEngine::clockNs)
    std::uint8_t port = 0;
    MidiBytes msg;
};

struct EngineEvent
{
    enum class Type : std::uint8_t
    {
        LateCallback,
        DspOverload,
        EmergencyComplete,
        SongEnded,
        CommandRejected   // e.g. output test requested while playing
    };

    Type type = Type::LateCallback;
    std::int64_t samplePosition = 0;
    float load = 0.0f;
};

/**
    Realtime audio engine core (docs/03). JUCE-independent so it can be unit-tested
    without an audio device.

    Threading contract:
      - prepare()/release(): non-realtime, with the device stopped.
      - postCommand(), popEvent(), setOutputRoute(), telemetry getters: message thread.
      - process(): audio thread only. Never allocates, locks, or blocks.
*/
class AudioEngine
{
public:
    static constexpr int maxOutputs = 64;
    static constexpr int maxTracks = 128;
    static constexpr int numBusKinds = 6;   // BusKind values
    static constexpr int maxMidiPorts = 16;
    static constexpr int midiQueueSize = 16384;

    /** The clock used for MidiOutEvent::dueNs (std::chrono::steady_clock). */
    static std::int64_t clockNs() noexcept;

    AudioEngine() = default;
    ~AudioEngine();

    void prepare (double sampleRate, int maxBlockSize, int numOutputs, int numInputs);
    void release();
    bool isPrepared() const noexcept { return prepared.load (std::memory_order_acquire); }

    /** Message thread -> audio thread. Returns false if the queue is full. */
    bool postCommand (const EngineCommand&) noexcept;
    /** Audio thread -> message thread. */
    bool popEvent (EngineEvent&) noexcept;

    /** Mirror of OutputRouting used by the realtime thread (BusCheck test and, from Phase 3, mixing). */
    void setRouting (const OutputRouting&) noexcept;

    void process (const float* const* inputs, int numInputs,
                  float* const* outputs, int numOutputs, int numSamples) noexcept;

    // --- song playback (message thread) ---

    /** Hands a prepared song to the engine. It is picked up by the audio thread at the
        next block where the transport is not playing; until then the old plan keeps
        playing. A plan built for another sample rate is ignored (silent). */
    void setPlan (std::unique_ptr<PlaybackPlan> plan, double planSampleRate);
    /** Frees plans the audio thread has finished with. Call regularly on the message thread. */
    void collectGarbage();
    bool hasPendingPlan() const noexcept { return pendingPlan.load (std::memory_order_acquire) != nullptr; }

    void setTrackGain (int slot, float linearGain) noexcept;
    void setTrackPan (int slot, float pan) noexcept;            // -1 .. +1
    void setTrackMute (int slot, bool) noexcept;
    void setTrackSolo (int slot, bool) noexcept;
    float takeTrackPeak (int slot) noexcept;

    void setMasterGain (float linear) noexcept       { masterGain.store (linear, std::memory_order_relaxed); }

    // --- external MIDI (docs/04) ---
    /** MIDI output thread (single consumer). */
    bool popMidi (MidiOutEvent& e) noexcept { return midiOut.pop (e); }
    /** Device output latency, so MIDI notes leave when their audio would reach the speakers. */
    /** Filter callback jitter out of MIDI timing (core/SampleClock). On by default. */
    void setClockSmoothing (bool on) noexcept { clockSmoothing.store (on, std::memory_order_relaxed); }
    /** Times the audio clock filter lost lock (dropouts, driver restarts). */
    std::uint64_t getClockRelockCount() const noexcept { return clockRelocks.load (std::memory_order_relaxed); }
    /** Tests only: replaces the wall clock read at each callback (set before playing). */
    void setTimeSourceForTesting (std::int64_t (*fn)()) noexcept { timeSource = fn; }

    void setOutputLatencyMs (double ms) noexcept { outputLatencyNs.store ((std::int64_t) (std::max (0.0, ms) * 1.0e6), std::memory_order_relaxed); }
    std::uint64_t getMidiDroppedCount() const noexcept { return midiDropped.load (std::memory_order_relaxed); }
    /** Notes currently sounding on a port (as sent by the engine). */
    int getActiveMidiNotes (int port) const noexcept { return port >= 0 && port < maxMidiPorts ? activeNotesTelemetry[(size_t) port].load (std::memory_order_relaxed) : 0; }
    /** Notes sounding in the plan's instrument track `index` (as sent by the engine). */
    int getActiveInstrumentNotes (int index) const noexcept { return index >= 0 && index < maxTracks ? instActiveTelemetry[(size_t) index].load (std::memory_order_relaxed) : 0; }
    std::uint64_t getInstrumentEventsDropped() const noexcept { return instrumentEventsDropped.load (std::memory_order_relaxed); }
    /** In-ear monitor mix: how much click, guide and MR (Main) go into the Monitor bus. */
    void setMonitorMix (float click, float guide, float main) noexcept;

    // --- telemetry (lock-free reads) ---
    EngineTransport getTransport() const noexcept { return (EngineTransport) transport.load (std::memory_order_acquire); }
    std::int64_t getPosition() const noexcept      { return position.load (std::memory_order_acquire); }
    bool isEmergencyStopped() const noexcept       { return emergencyLatched.load (std::memory_order_acquire); }
    TestSignal getTestSignal() const noexcept      { return (TestSignal) testSignal.load (std::memory_order_acquire); }
    int getTestOutput() const noexcept             { return testOutput.load (std::memory_order_acquire); }
    double getSampleRate() const noexcept          { return sampleRate; }
    int getNumOutputs() const noexcept             { return numOuts; }
    float getLoad() const noexcept                 { return loadTelemetry.load (std::memory_order_relaxed); }
    /** Worst single-callback processing time / block time since the device started. */
    float getPeakLoad() const noexcept             { return peakLoadTelemetry.load (std::memory_order_relaxed); }
    std::uint64_t getLateCallbackCount() const noexcept { return lateTelemetry.load (std::memory_order_relaxed); }
    std::uint64_t getOverloadCount() const noexcept     { return overloadTelemetry.load (std::memory_order_relaxed); }
    std::uint64_t getCallbackCount() const noexcept     { return callbacks.load (std::memory_order_relaxed); }

    /** Peak since the last call (single consumer: the UI meter timer). */
    float takeOutputPeak (int output) noexcept;

private:
    void renderChunk (float* const* outputs, int numOutputs, int numSamples) noexcept;
    void renderTracks (int numSamples) noexcept;
    void renderClick (int numSamples) noexcept;
    void renderMidi (int numSamples) noexcept;
    void renderInstruments (int numSamples) noexcept;
    /** Note Off / CC64 0 / CC123 into every instrument that has sounding notes (output discarded). */
    void releaseInstrumentNotes() noexcept;
    /** Note Off for every sounding note, CC64 0 and CC123 on used channels, all ports, sent now. */
    void releaseMidiNotes() noexcept;
    void pushMidi (std::int64_t dueNs, int port, const MidiBytes&) noexcept;
    bool anySoloActive() const noexcept;
    void handleCommand (const EngineCommand&) noexcept;
    void pushEvent (EngineEvent::Type, float load = 0.0f) noexcept;
    void renderTest (float* const* outputs, int numOutputs, int start, int numSamples) noexcept;
    float nextNoise() noexcept;

    // configuration (written in prepare only)
    double sampleRate = 48000.0;
    int maxBlock = 0;
    int numOuts = 0, numIns = 0;
    std::atomic<bool> prepared { false };

    SpscQueue<EngineCommand, 256> commands;
    SpscQueue<EngineEvent, 256> events;

    struct RouteSlot
    {
        std::atomic<int> bus { -1 };      // BusKind as int, -1 = none
        std::atomic<int> channel { 0 };   // 0/1, or BusChannel::monoSum
        std::atomic<bool> isPA { false };
    };

    struct TrackParams
    {
        std::atomic<float> gain { 1.0f };
        std::atomic<float> pan { 0.0f };
        std::atomic<bool> mute { false };
        std::atomic<bool> solo { false };
        std::atomic<float> peak { 0.0f };
    };
    std::array<TrackParams, maxTracks> trackParams;
    std::array<float, maxTracks> smoothL {}, smoothR {};   // audio thread only

    // Plans: message thread -> pendingPlan -> audio thread (currentPlan) -> retired -> message thread deletes.
    std::atomic<PlaybackPlan*> pendingPlan { nullptr };
    std::atomic<double> pendingPlanRate { 0.0 };
    PlaybackPlan* currentPlan = nullptr;
    double currentPlanRate = 0.0;
    bool currentPlanValid = false;
    SpscQueue<PlaybackPlan*, 16> retiredPlans;

    // Bus buffers [kind][channel], sized in prepare().
    std::array<std::array<std::vector<float>, 2>, numBusKinds> busBuffers;
    std::atomic<float> masterGain { 1.0f };
    std::atomic<float> monitorClick { 1.0f }, monitorGuide { 1.0f }, monitorMain { 0.0f };

    // Click voice (audio thread only)
    int clickVoicePos = -1;
    bool clickVoiceAccent = false;
    std::int64_t lastRenderEnd = -1;
    std::array<RouteSlot, maxOutputs> routes;
    std::array<std::atomic<float>, maxOutputs> peaks {};

    // transport (audio thread owns, mirrored to atomics)
    std::atomic<int> transport { (int) EngineTransport::Stopped };
    std::atomic<std::int64_t> position { 0 };
    std::int64_t songLength = 0;

    // fades: stop/emergency ramp the master gain to zero over ~5 ms before stopping
    float fadeGain = 1.0f;
    float fadeStep = 0.0f;
    enum class FadeAction { None, Stop, Emergency } fadeAction = FadeAction::None;
    // PLAY received during a stop fade: finish the fade, swap in a waiting plan, then play.
    bool playAfterStop = false;
    std::int64_t playAfterStopPosition = 0;
    std::atomic<bool> emergencyLatched { false };

    // test signals
    std::atomic<int> testSignal { (int) TestSignal::None };
    std::atomic<int> testOutput { -1 };
    float testGain = 0.1f;
    double testPhase = 0.0;
    std::int64_t testClock = 0;     // samples since test start
    std::uint32_t noiseState = 0x12345678u;
    float pinkB0 = 0.0f, pinkB1 = 0.0f, pinkB2 = 0.0f;

    // external MIDI (audio thread only, except the queue and telemetry)
    SpscQueue<MidiOutEvent, midiQueueSize> midiOut;
    std::array<ActiveNoteTracker, maxMidiPorts> midiNotes;
    std::array<std::size_t, maxTracks> midiCursor {};
    std::int64_t midiExpectedPos = -1;
    std::int64_t chunkStartNs = 0;
    std::atomic<std::int64_t> outputLatencyNs { 0 };
    SampleClock sampleClock;                         // audio thread only
    std::atomic<bool> clockSmoothing { true };
    std::atomic<std::uint64_t> clockRelocks { 0 };
    std::int64_t (*timeSource)() = nullptr;
    std::atomic<std::uint64_t> midiDropped { 0 };
    std::array<std::atomic<int>, maxMidiPorts> activeNotesTelemetry {};

    // software instruments (audio thread only)
    static constexpr int maxInstrumentEvents = 2048;   // per track per block; more are dropped
    std::array<MidiBytes, maxInstrumentEvents> instMsgs {};
    std::array<int, maxInstrumentEvents> instOffsets {};
    std::array<std::size_t, maxTracks> instCursor {};
    std::array<ActiveNoteTracker, maxTracks> instNotes;
    std::int64_t instExpectedPos = -1;
    std::vector<float> instL, instR;
    std::atomic<std::uint64_t> instrumentEventsDropped { 0 };
    std::array<std::atomic<int>, maxTracks> instActiveTelemetry {};

    XrunDetector xruns;
    std::atomic<float> loadTelemetry { 0.0f }, peakLoadTelemetry { 0.0f };
    std::atomic<std::uint64_t> lateTelemetry { 0 }, overloadTelemetry { 0 }, callbacks { 0 };
};

} // namespace b4nd
