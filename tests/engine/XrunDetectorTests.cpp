#include "TestHarness.h"
#include "core/XrunDetector.h"

using namespace b4nd;

static constexpr std::int64_t ms = 1'000'000;

TEST_CASE ("XrunDetector: regular callbacks produce no xruns")
{
    XrunDetector d;
    d.prepare (48000.0);
    // 256 samples @48k = 5.333 ms, 1 ms processing
    std::int64_t t = 0;
    for (int i = 0; i < 1000; ++i)
    {
        CHECK (d.onCallback (t, t + 1 * ms, 256) == XrunDetector::None);
        t += 5'333'333;
    }
    CHECK (d.getLateCount() == 0);
    CHECK (d.getOverloadCount() == 0);
    CHECK (d.getLoad() > 0.15 && d.getLoad() < 0.22);
}

TEST_CASE ("XrunDetector: a late callback is counted once")
{
    XrunDetector d;
    d.prepare (48000.0);
    std::int64_t t = 0;
    d.onCallback (t, t + ms, 256);
    t += 5'333'333;
    d.onCallback (t, t + ms, 256);
    t += 20 * ms;   // driver stalled ~4 blocks
    CHECK ((d.onCallback (t, t + ms, 256) & XrunDetector::LateCallback) != 0);
    t += 5'333'333;
    CHECK (d.onCallback (t, t + ms, 256) == XrunDetector::None);
    CHECK (d.getLateCount() == 1);
}

TEST_CASE ("XrunDetector: processing over 80% of the block is an overload")
{
    XrunDetector d;
    d.prepare (48000.0);
    CHECK ((d.onCallback (0, 5 * ms, 256) & XrunDetector::DspOverload) != 0);   // 5 ms of 5.33 ms
    CHECK (d.getOverloadCount() == 1);
    CHECK (d.getPeakLoad() > 0.9);
}

TEST_CASE ("XrunDetector: small jitter tolerated (WASAPI-style timing)")
{
    XrunDetector d;
    d.prepare (48000.0);
    std::int64_t t = 0;
    for (int i = 0; i < 200; ++i)
    {
        d.onCallback (t, t + ms, 480);
        t += (i % 2 == 0) ? 10 * ms + 900'000 : 10 * ms - 900'000;   // 10 ms blocks, ±0.9 ms jitter
    }
    CHECK (d.getLateCount() == 0);
}
