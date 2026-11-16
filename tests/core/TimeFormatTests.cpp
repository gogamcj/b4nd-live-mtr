#include "TestHarness.h"
#include "core/TimeFormat.h"

using namespace b4nd;

TEST_CASE ("TimeFormat: seconds")
{
    CHECK (formatSeconds (0.0) == "0:00.000");
    CHECK (formatSeconds (65.4321) == "1:05.432");
    CHECK (formatSeconds (3725.0, false) == "1:02:05");
    CHECK (formatSeconds (-3.5) == "-0:03.500");
    CHECK (formatSeconds (59.9996, false) == "1:00");
}

TEST_CASE ("TimeFormat: bar.beat.tick is 1-based")
{
    BarBeat bb;
    bb.bar = 0; bb.beatInBar = 0; bb.fraction = 0.0;
    CHECK (formatBarBeat (bb) == "1.1.000");
    bb.bar = 11; bb.beatInBar = 2; bb.fraction = 0.5;
    CHECK (formatBarBeat (bb) == "12.3.480");
}

TEST_CASE ("TimeFormat: setlist total ignores invalid lengths")
{
    const double l[] = { 200.0, 185.5, -1.0, 240.0 };
    CHECK_NEAR (totalSeconds (l, 4), 625.5, 1e-12);
}
