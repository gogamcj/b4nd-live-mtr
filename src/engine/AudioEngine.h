#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "core/OutputRouting.h"
#include "core/SpscQueue.h"
#include "core/XrunDetector.h"

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

    AudioEngine() = default;

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

    // --- telemetry (lock-free reads) ---
    EngineTransport getTransport() const noexcept { return (EngineTransport) transport.load (std::memory_order_acquire); }
    std::int64_t getPosition() const noexcept      { return position.load (std::memory_order_acquire); }
    bool isEmergencyStopped() const noexcept       { return emergencyLatched.load (std::memory_order_acquire); }
    TestSignal getTestSignal() const noexcept      { return (TestSignal) testSignal.load (std::memory_order_acquire); }
    int getTestOutput() const noexcept             { return testOutput.load (std::memory_order_acquire); }
    double getSampleRate() const noexcept          { return sampleRate; }
    int getNumOutputs() const noexcept             { return numOuts; }
    float getLoad() const noexcept                 { return loadTelemetry.load (std::memory_order_relaxed); }
    std::uint64_t getLateCallbackCount() const noexcept { return lateTelemetry.load (std::memory_order_relaxed); }
    std::uint64_t getOverloadCount() const noexcept     { return overloadTelemetry.load (std::memory_order_relaxed); }
    std::uint64_t getCallbackCount() const noexcept     { return callbacks.load (std::memory_order_relaxed); }

    /** Peak since the last call (single consumer: the UI meter timer). */
    float takeOutputPeak (int output) noexcept;

private:
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
        std::atomic<bool> isPA { false };
    };
    std::array<RouteSlot, maxOutputs> routes;
    std::array<std::atomic<float>, maxOutputs> peaks {};

    // transport (audio thread owns, mirrored to atomics)
    std::atomic<int> transport { (int) EngineTransport::Stopped };
    std::atomic<std::int64_t> position { 0 };
    std::int64_t songLength = 0;

    // fades: stop/emergency ramp the master gain to zero over ~5 ms before stopping
    float masterGain = 1.0f;
    float fadeStep = 0.0f;
    enum class FadeAction { None, Stop, Emergency } fadeAction = FadeAction::None;
    std::atomic<bool> emergencyLatched { false };

    // test signals
    std::atomic<int> testSignal { (int) TestSignal::None };
    std::atomic<int> testOutput { -1 };
    float testGain = 0.1f;
    double testPhase = 0.0;
    std::int64_t testClock = 0;     // samples since test start
    std::uint32_t noiseState = 0x12345678u;
    float pinkB0 = 0.0f, pinkB1 = 0.0f, pinkB2 = 0.0f;

    XrunDetector xruns;
    std::atomic<float> loadTelemetry { 0.0f };
    std::atomic<std::uint64_t> lateTelemetry { 0 }, overloadTelemetry { 0 }, callbacks { 0 };
};

} // namespace b4nd
