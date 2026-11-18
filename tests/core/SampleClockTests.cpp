// Audio clock -> wall clock filter (docs/04 §4): callback jitter must not reach MIDI timing.

#include "TestHarness.h"
#include "core/SampleClock.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

using namespace b4nd;

namespace
{
/** Simulated device: true block starts are perfectly regular at `realRate`; the callback
    is observed late by a random jitter (and sometimes a burst). */
struct Sim
{
    double realRate;
    int block;
    double jitterMs;
    std::mt19937 rng { 1234 };
    double trueNs = 1.0e12;   // arbitrary epoch

    std::int64_t nextMeasured()
    {
        std::uniform_real_distribution<double> u (0.0, jitterMs * 1.0e6);
        return (std::int64_t) (trueNs + u (rng));
    }
    void advance() { trueNs += block * 1.0e9 / realRate; }
};
} // namespace

TEST_CASE ("SampleClock: cuts random callback jitter at least 3x (+-1 ms -> under +-0.35 ms)")
{
    SampleClock clock;
    clock.prepare (48000.0);
    Sim sim { 48000.0, 256, 2.0 };
    double maxRawErr = 0.0, maxErr = 0.0;
    double meanErr = 0.0;
    int counted = 0;
    for (int i = 0; i < 4000; ++i)   // ~21 s
    {
        const auto m = sim.nextMeasured();
        const auto t = clock.onCallback (m, sim.block);
        if (i > 1000)   // after settling
        {
            // The filter cannot know the constant part of the lateness (it looks like a later
            // clock); only the variation matters for MIDI: compare against the mean.
            meanErr += (double) t - sim.trueNs;
            ++counted;
        }
        sim.advance();
    }
    meanErr /= counted;
    Sim sim2 { 48000.0, 256, 2.0 };
    SampleClock c2;
    c2.prepare (48000.0);
    for (int i = 0; i < 4000; ++i)
    {
        const auto m = sim2.nextMeasured();
        const auto t = c2.onCallback (m, sim2.block);
        if (i > 1000)
        {
            maxErr = std::max (maxErr, std::abs ((double) t - sim2.trueNs - meanErr));
            maxRawErr = std::max (maxRawErr, std::abs ((double) m - sim2.trueNs - 1.0e6));
        }
        sim2.advance();
    }
    std::printf ("    raw jitter +-%.2f ms, filtered +-%.3f ms\n", maxRawErr / 1.0e6, maxErr / 1.0e6);
    CHECK (maxRawErr > 0.9e6);
    CHECK (maxErr < 0.35e6);
}

TEST_CASE ("SampleClock: learns a device clock that is 0.05 % off")
{
    SampleClock clock;
    clock.prepare (48000.0);
    Sim sim { 48024.0, 256, 0.5 };
    double sum = 0.0;
    int n = 0;
    std::int64_t first = 0, last = 0;
    for (int i = 0; i < 12000; ++i)
    {
        const auto t = clock.onCallback (sim.nextMeasured(), sim.block);
        if (i >= 4000)
        {
            sum += clock.getNsPerSample();
            ++n;
            if (i == 4000) first = t;
            last = t;
        }
        sim.advance();
    }
    const double realNs = 1.0e9 / 48024.0;
    CHECK (std::abs (sum / n - realNs) / realNs < 2.0e-5);
    // Block start times advance at the real rate, not the nominal 48000 Hz.
    const double spacing = (double) (last - first) / ((n - 1) * 256.0);
    CHECK (std::abs (spacing - realNs) / realNs < 2.0e-5);
}

TEST_CASE ("SampleClock: relocks after a dropout instead of drifting")
{
    SampleClock clock;
    clock.prepare (48000.0);
    Sim sim { 48000.0, 256, 0.2 };
    for (int i = 0; i < 500; ++i) { clock.onCallback (sim.nextMeasured(), sim.block); sim.advance(); }
    CHECK (clock.getRelockCount() == 0);
    sim.trueNs += 50.0e6;   // the driver stalled for 50 ms
    const auto m = sim.nextMeasured();
    const auto t = clock.onCallback (m, sim.block);
    CHECK (clock.getRelockCount() == 1);
    CHECK (t == m);   // follows the measurement at once
    sim.advance();
    for (int i = 0; i < 200; ++i) { clock.onCallback (sim.nextMeasured(), sim.block); sim.advance(); }
    CHECK (clock.getRelockCount() == 1);
}

TEST_CASE ("SampleClock: variable block sizes keep sample-accurate spacing")
{
    SampleClock clock;
    clock.prepare (48000.0);
    double trueNs = 5.0e11;
    std::int64_t last = 0;
    int lastN = 0;
    double worst = 0.0;
    const int sizes[] = { 256, 192, 320, 256, 128, 384 };
    for (int i = 0; i < 3000; ++i)
    {
        const int n = sizes[i % 6];
        const auto t = clock.onCallback ((std::int64_t) trueNs, n);
        if (i > 500)
            worst = std::max (worst, std::abs ((double) (t - last) - lastN * 1.0e9 / 48000.0));
        last = t;
        lastN = n;
        trueNs += n * 1.0e9 / 48000.0;
    }
    CHECK (worst < 2.0e4);   // 20 us
}
