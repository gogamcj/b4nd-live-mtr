#pragma once

#include <cstdint>

namespace b4nd
{

/**
    Maps the audio sample clock to wall-clock time without the callback jitter (docs/04 §4).

    Audio callbacks arrive with jitter (drivers deliver blocks in bursts, the OS schedules the
    thread late), but the samples themselves leave the converter at a perfectly steady rate.
    Timing external MIDI from the raw callback time copies that jitter onto the MIDI wire.
    This is a second-order delay-locked loop (F. Adriaensen, "Using a DLL to filter time", 2005):
    it tracks the true block start time and the real duration of a sample (the interface clock
    is never exactly 48000 Hz by the PC's clock).

    Realtime safe: no allocation, no locks. One instance per audio thread.
*/
class SampleClock
{
public:
    /** bandwidthHz: how fast it follows real changes (lower = smoother, slower to settle).
        0.5 Hz turns 2 ms of random callback lateness into about +-0.3 ms. */
    void prepare (double sampleRate, double bandwidthHz = 0.5) noexcept;
    /** Forget the lock (device restart, dropout). The next callback starts again. */
    void reset() noexcept { locked = false; }

    /** Call once per audio callback with the measured time (ns) and its sample count.
        Returns the filtered time (ns) at which this block's first sample was due. */
    std::int64_t onCallback (std::int64_t measuredNs, int numSamples) noexcept;

    bool isLocked() const noexcept { return locked; }
    /** Filtered sample duration in ns (1e9 / real sample rate). Noisy from block to block;
        its average is the real rate. */
    double getNsPerSample() const noexcept { return nsPerSample; }
    /** Times the loop lost lock (error too big: dropout, device glitch). */
    std::uint64_t getRelockCount() const noexcept { return relocks; }

private:
    double sampleRate = 48000.0, nominalNsPerSample = 1.0e9 / 48000.0, nsPerSample = 1.0e9 / 48000.0;
    double bandwidth = 1.0;
    double predictedNs = 0.0;   // when the next block should start
    bool locked = false;
    std::uint64_t relocks = 0;
};

} // namespace b4nd
