#pragma once

#include <cstdint>

namespace b4nd
{

/**
    Detects audio dropouts from callback timing (docs/03 §6). Pure logic, realtime-safe.

    - Late callback: the gap since the previous callback start exceeds
      lateFactor x the block duration (plus a small absolute tolerance).
      This catches driver-level underruns even when the driver does not report them.
    - DSP overload: our processing time exceeds overloadFactor x the block duration.
      The audio will glitch on the next block if this persists.
*/
class XrunDetector
{
public:
    enum Result : std::uint32_t
    {
        None         = 0,
        LateCallback = 1u << 0,
        DspOverload  = 1u << 1
    };

    void prepare (double sampleRate, double lateFactor = 1.5, double overloadFactor = 0.8,
                  std::int64_t toleranceNs = 1'000'000) noexcept;
    void reset() noexcept;

    /** Call once per callback with monotonic timestamps (ns) of callback start and end. */
    std::uint32_t onCallback (std::int64_t startNs, std::int64_t endNs, int numSamples) noexcept;

    std::uint64_t getLateCount() const noexcept     { return lateCount; }
    std::uint64_t getOverloadCount() const noexcept { return overloadCount; }
    /** Smoothed DSP load 0..1+ (processing time / block time). */
    double getLoad() const noexcept                 { return load; }
    double getPeakLoad() const noexcept             { return peakLoad; }

private:
    double sampleRate = 48000.0;
    double lateFactor = 1.5, overloadFactor = 0.8;
    std::int64_t toleranceNs = 1'000'000;
    std::int64_t lastStartNs = -1;
    std::int64_t lastExpectedNs = 0;
    std::uint64_t lateCount = 0, overloadCount = 0;
    double load = 0.0, peakLoad = 0.0;
};

} // namespace b4nd
