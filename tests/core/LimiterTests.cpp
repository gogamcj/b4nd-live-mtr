// Main (PA) bus limiter (docs/06 §6): ceiling held without hard clipping, delay, transparency.

#include "TestHarness.h"
#include "core/Limiter.h"

#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace b4nd;

namespace
{
constexpr double sr = 48000.0;
constexpr float ceilingLin = 0.96605f;   // -0.3 dBFS
}

TEST_CASE ("Limiter: quiet signal passes unchanged, only delayed by the look-ahead")
{
    Limiter lim;
    lim.prepare (sr);
    lim.setCeilingDb (-0.3f);
    CHECK (lim.getLatencySamples() == 48);
    std::vector<float> l (4800), r (4800);
    for (int i = 0; i < 4800; ++i) { l[(size_t) i] = 0.5f * (float) std::sin (i * 0.05); r[(size_t) i] = -l[(size_t) i]; }
    auto inL = l;
    lim.process (l.data(), r.data(), 4800);
    float err = 0.0f;
    for (int i = 48; i < 4800; ++i) err = std::max (err, std::abs (l[(size_t) i] - inL[(size_t) (i - 48)]));
    CHECK (err < 1.0e-6f);
    CHECK (lim.takeMinGain() == 1.0f);
}

TEST_CASE ("Limiter: hot peaks never pass the ceiling and the look-ahead catches them (no hard clip)")
{
    Limiter lim;
    lim.prepare (sr);
    lim.setCeilingDb (-0.3f);
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    float maxOut = 0.0f;
    std::vector<float> l (256), r (256);
    for (int b = 0; b < 2000; ++b)
    {
        for (int i = 0; i < 256; ++i)
        {
            const int k = b * 256 + i;
            float v = 0.4f * (float) std::sin (k * 0.03) + 0.1f * u (rng);
            if (k % 9000 == 0) v = 4.0f;                       // single-sample spike (+12 dBFS)
            if ((k / 24000) % 3 == 1) v *= 3.0f;               // a loud section
            l[(size_t) i] = v;
            r[(size_t) i] = (k % 7000 == 0) ? -6.0f : v * 0.8f;   // spike on one side only
        }
        lim.process (l.data(), r.data(), 256);
        for (int i = 0; i < 256; ++i)
            maxOut = std::max ({ maxOut, std::abs (l[(size_t) i]), std::abs (r[(size_t) i]) });
    }
    CHECK (maxOut <= ceilingLin + 1.0e-5f);
    CHECK (lim.safetyClips == 0);
    CHECK (lim.takeMinGain() < 0.5f);
}

TEST_CASE ("Limiter: NaN and Inf become silence, the gain recovers after a peak")
{
    Limiter lim;
    lim.prepare (sr);
    std::vector<float> l (512, 0.0f), r (512, 0.0f);
    l[10] = std::numeric_limits<float>::quiet_NaN();
    r[20] = std::numeric_limits<float>::infinity();
    l[30] = 2.0f;
    lim.process (l.data(), r.data(), 512);
    for (int i = 0; i < 512; ++i) { CHECK (std::isfinite (l[(size_t) i])); CHECK (std::isfinite (r[(size_t) i])); }
    // One second later a 0.5 signal passes at (almost) full level again.
    std::vector<float> a (48000, 0.0f), b (48000, 0.0f);
    lim.process (a.data(), b.data(), 48000);
    std::vector<float> x (480, 0.5f), y (480, 0.5f);
    lim.process (x.data(), y.data(), 480);
    CHECK (x[400] > 0.49f);
}
