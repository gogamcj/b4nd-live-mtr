#pragma once

#include <array>
#include <cstdint>

namespace b4nd
{

/**
    Stereo-linked look-ahead peak limiter for the Main (PA) bus (docs/06 §6).

    The gain needed for each incoming sample is held over the look-ahead window, released
    slowly, then averaged over the same window, so the gain is already down when a peak leaves
    the delay line: no overshoot and no hard-clip distortion in normal use. A final hard clip at
    the ceiling and a NaN/Inf guard stay as the last line of defence.

    Adds `getLatencySamples()` of delay. Audio thread only after prepare(); no allocation.
*/
class Limiter
{
public:
    static constexpr int maxLookahead = 2048;

    void prepare (double sampleRate, double lookaheadMs = 1.0, double releaseMs = 80.0) noexcept;
    void reset() noexcept;
    void setCeilingDb (float db) noexcept;

    /** In place, two channels (R may equal L for mono). */
    void process (float* left, float* right, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return lookahead; }
    /** Lowest gain since the last call (1 = no limiting), for a GR meter. */
    float takeMinGain() noexcept { const float g = minGain; minGain = 1.0f; return g; }

private:
    double sr = 48000.0;
    int lookahead = 48;
    float ceiling = 0.966f;   // -0.3 dBFS
    double releaseCoef = 0.0;

    std::array<float, maxLookahead> delayL {}, delayR {};
    int delayPos = 0;

    // sliding-window minimum of the required gain (monotonic queue on a ring)
    std::array<float, maxLookahead> qVal {};
    std::array<std::int64_t, maxLookahead> qIdx {};
    int qHead = 0, qTail = 0;   // [head, tail) modulo maxLookahead
    std::int64_t index = 0;

    float released = 1.0f;
    std::array<float, maxLookahead> box {};
    double boxSum = 0.0;
    int boxPos = 0;
    float minGain = 1.0f;
public:
    /** Samples the final hard clip had to catch (should stay 0: the look-ahead does the work). */
    std::uint64_t safetyClips = 0;
};

} // namespace b4nd
