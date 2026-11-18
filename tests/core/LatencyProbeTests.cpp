// Latency measurement (docs/04 §4): loopback round trip and module MIDI-to-sound delay.

#include "TestHarness.h"
#include "core/LatencyProbe.h"

#include <cmath>
#include <deque>
#include <random>
#include <vector>

using namespace b4nd;

namespace
{
constexpr double sr = 48000.0;

/** Output cabled to input through a delay line (with optional noise and a soft low-pass,
    like converter filters). */
struct Loopback
{
    int delay;
    float noise = 0.0f;
    float smooth = 0.0f;   // 0 = none, closer to 1 = duller
    std::deque<float> line;
    std::mt19937 rng { 7 };
    float lp = 0.0f;
    explicit Loopback (int d) : delay (d), line ((size_t) d, 0.0f) {}

    void run (const float* out, float* in, int n)
    {
        std::uniform_real_distribution<float> u (-1.0f, 1.0f);
        for (int i = 0; i < n; ++i)
        {
            line.push_back (out[i]);
            const float y = line.front();
            line.pop_front();
            lp = smooth * lp + (1.0f - smooth) * y;
            in[i] = lp + noise * u (rng);
        }
    }
};

LatencyProbe::State runLoopback (LatencyProbe& p, Loopback& lb, int block, double maxSec = 20.0)
{
    std::vector<float> out ((size_t) block), in ((size_t) block, 0.0f);
    for (int b = 0; b < (int) (maxSec * sr / block) && p.getState() == LatencyProbe::State::Running; ++b)
    {
        std::fill (out.begin(), out.end(), 0.0f);
        p.process (in.data(), out.data(), block);   // input of this callback was recorded before its output
        lb.run (out.data(), in.data(), block);
    }
    return p.getState();
}
}

TEST_CASE ("LatencyProbe: loopback round trip is measured to the sample")
{
    // The probe sees each callback's input one block after the device recorded it, which is
    // part of the real round trip (the cable delay here plus one block).
    for (int block : { 64, 256, 1024 })
    {
        LatencyProbe p;
        LatencyProbe::Settings s;
        p.start (s);
        Loopback lb (3 * 256 + 17);
        lb.noise = 0.0005f;   // about -66 dBFS hiss
        CHECK (runLoopback (p, lb, block) == LatencyProbe::State::Done);
        CHECK (p.getValidPings() == 5);
        CHECK (p.getDelaySamples() == 3 * 256 + 17 + block);
        CHECK (p.getSpreadSamples() == 0);
    }
}

TEST_CASE ("LatencyProbe: a dull converter path still lands within a few samples")
{
    LatencyProbe p;
    p.start ({});
    Loopback lb (500);
    lb.smooth = 0.6f;
    lb.noise = 0.001f;
    CHECK (runLoopback (p, lb, 128) == LatencyProbe::State::Done);
    CHECK (p.getDelaySamples() >= 500 + 128);
    CHECK (p.getDelaySamples() <= 500 + 128 + 3);
}

TEST_CASE ("LatencyProbe: no cable, noise and missing input fail clearly")
{
    {
        LatencyProbe p;
        p.start ({});
        std::vector<float> out (256), in (256, 0.0f);
        for (int b = 0; b < 2000 && p.getState() == LatencyProbe::State::Running; ++b)
            p.process (in.data(), out.data(), 256);   // input never hears the burst
        CHECK (p.getState() == LatencyProbe::State::Failed);
        CHECK (p.getFailure() == LatencyProbe::Failure::NoSignal);
    }
    {
        LatencyProbe p;
        p.start ({});
        Loopback lb (300);
        lb.noise = 0.3f;   // a hot open input or feedback
        CHECK (runLoopback (p, lb, 256) == LatencyProbe::State::Failed);
        CHECK (p.getFailure() == LatencyProbe::Failure::Noisy);
    }
    {
        LatencyProbe p;
        p.start ({});
        std::vector<float> out (256);
        p.process (nullptr, out.data(), 256);
        CHECK (p.getFailure() == LatencyProbe::Failure::NoSignal);
    }
    {
        LatencyProbe p;
        p.start ({});
        std::vector<float> out (256), in (256, 0.0f);
        p.process (in.data(), out.data(), 256);
        p.cancel();
        CHECK (p.getFailure() == LatencyProbe::Failure::Cancelled);
        // Nothing more is written once cancelled.
        p.process (in.data(), out.data(), 256);
        for (float v : out) CHECK (v == 0.0f);
    }
}

TEST_CASE ("LatencyProbe: module mode measures the delay to a ringing sound, and rejects an unstable one")
{
    // The caller (engine) sends a MIDI note at each stimulus; the simulated module answers
    // after `delay` +- jitter with a slowly decaying tone (piano-like tail).
    auto run = [] (double jitterMs, int& delayOut) {
        LatencyProbe p;
        LatencyProbe::Settings s;
        s.writeBurst = false;
        s.maxSpreadSec = 0.003;
        p.start (s);
        std::mt19937 rng (3);
        std::uniform_real_distribution<double> u (-jitterMs, jitterMs);
        std::vector<float> out (256, 0.0f), in (256, 0.0f);
        std::int64_t t = 0, soundAt = -1;
        const int moduleDelay = 288;   // 6 ms
        for (int b = 0; b < 20000 && p.getState() == LatencyProbe::State::Running; ++b, t += 256)
        {
            for (int i = 0; i < 256; ++i)
            {
                const auto k = t + i - soundAt;
                in[(size_t) i] = soundAt >= 0 && k >= 0 ? 0.4f * (float) std::exp (-(double) k / (0.12 * sr)) * (float) std::sin (k * 0.05) : 0.0f;
            }
            const int st = p.process (in.data(), out.data(), 256);
            for (float v : out) CHECK (v == 0.0f);   // module mode never writes audio
            if (st >= 0)
                soundAt = t + st + moduleDelay + (std::int64_t) (u (rng) * sr / 1000.0);
        }
        delayOut = p.getDelaySamples();
        return p.getState();
    };
    int d = 0;
    CHECK (run (0.4, d) == LatencyProbe::State::Done);
    CHECK (std::abs (d - 288) <= 25);
    CHECK (run (6.0, d) == LatencyProbe::State::Failed);
}
