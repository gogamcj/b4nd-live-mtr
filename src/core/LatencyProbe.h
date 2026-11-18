#pragma once

#include <array>
#include <cstdint>

namespace b4nd
{

/**
    Measures a delay by sending a stimulus and finding its onset on an input (docs/04 §4).

    Used in two ways by the engine:
      - audio loopback: the probe writes a short burst to an output that is cabled back to an
        input; the delay is the interface's real round trip (output + input latency).
      - external module: the engine sends a MIDI test note at the stimulus sample, the module's
        audio comes back on an input; delay = MIDI-to-sound plus the round trip.

    A run is several pings: settle (quiet input), stimulus, listen for the onset. The result is
    the median delay with its spread. Audio thread only, no allocation, no locks.
*/
class LatencyProbe
{
public:
    static constexpr int maxPings = 16;

    enum class State : int { Idle, Running, Done, Failed };
    enum class Failure : int { None, NoSignal, Noisy, Unstable, Cancelled, BadSetup };

    struct Settings
    {
        double sampleRate = 48000.0;
        int pings = 5;
        float level = 0.25f;            // burst amplitude (audio loopback)
        double listenSec = 0.75;        // longest delay accepted
        double maxSpreadSec = 0.001;    // pings must agree this well
        bool writeBurst = true;         // false: the stimulus is the caller's MIDI note
    };

    /** Starts a run (replaces any running one). */
    void start (const Settings&) noexcept;
    void cancel() noexcept;
    /** Ends a run (or marks a run that never started) as failed for the caller's reason. */
    void abort (Failure why) noexcept { state = State::Failed; failure = why; burstPos = -1; }

    /** One chunk. `in` may be null (no input: the run fails with NoSignal). The burst is added
        into `out` when writing bursts (`out` may be null). Returns the offset in this chunk
        where a stimulus starts, or -1. */
    int process (const float* in, float* out, int numSamples) noexcept;

    State getState() const noexcept       { return state; }
    Failure getFailure() const noexcept   { return failure; }
    /** Median delay in samples (valid when Done, and when Failed with Unstable). */
    int getDelaySamples() const noexcept  { return median; }
    int getSpreadSamples() const noexcept { return spread; }
    int getValidPings() const noexcept    { return valid; }
    int getPingsDone() const noexcept     { return pingIndex; }

private:
    enum class Phase { Noise, Settle, Listen };
    void finish() noexcept;

    Settings s;
    State state = State::Idle;
    Failure failure = Failure::None;
    Phase phase = Phase::Noise;
    std::int64_t clock = 0;          // samples since start
    std::int64_t phaseStart = 0;
    std::int64_t quietSince = 0;
    std::int64_t stimulusAt = -1;
    float noisePeak = 0.0f, threshold = 0.0f;
    int burstLen = 24, burstPos = -1;
    int pingIndex = 0, valid = 0, median = -1, spread = 0;
    std::array<int, maxPings> delays {};
};

} // namespace b4nd
