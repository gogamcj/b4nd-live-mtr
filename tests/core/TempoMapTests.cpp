#include "TestHarness.h"
#include "core/TempoMap.h"
#include <cmath>

using namespace b4nd;

TEST_CASE ("TempoMap: default is 120 BPM 4/4")
{
    TempoMap m;
    CHECK_NEAR (m.secondsAtBeat (4.0), 2.0, 1e-12);
    CHECK_NEAR (m.beatAtSeconds (2.0), 4.0, 1e-12);
    CHECK_NEAR (m.beatAtBar (3), 12.0, 1e-12);
}

TEST_CASE ("TempoMap: step tempo change keeps time continuous")
{
    TempoMap m;
    CHECK (m.setTempoPoints ({ { 0.0, 120.0 }, { 8.0, 60.0 } }));
    // 8 beats @120 = 4 s, then each beat 1 s.
    CHECK_NEAR (m.secondsAtBeat (8.0), 4.0, 1e-12);
    CHECK_NEAR (m.secondsAtBeat (10.0), 6.0, 1e-12);
    CHECK_NEAR (m.beatAtSeconds (6.0), 10.0, 1e-12);
    CHECK_NEAR (m.beatAtSeconds (3.0), 6.0, 1e-12);
    CHECK_NEAR (m.bpmAtBeat (7.999), 120.0, 1e-12);
    CHECK_NEAR (m.bpmAtBeat (8.0), 60.0, 1e-12);
}

TEST_CASE ("TempoMap: round trip beat <-> seconds over a multi-change map")
{
    TempoMap m;
    CHECK (m.setTempoPoints ({ { 0.0, 97.3 }, { 16.0, 131.0 }, { 64.0, 88.8 }, { 100.5, 174.0 } }));
    double worst = 0.0;
    for (double b = 0.0; b < 300.0; b += 0.37)
    {
        const double back = m.beatAtSeconds (m.secondsAtBeat (b));
        worst = std::max (worst, std::fabs (back - b));
    }
    CHECK (worst < 1e-9);
}

TEST_CASE ("TempoMap: unsorted points and missing zero point are normalised")
{
    TempoMap m;
    CHECK (m.setTempoPoints ({ { 16.0, 90.0 }, { 4.0, 140.0 } }));
    const auto& p = m.getTempoPoints();
    CHECK (p.size() == 3);
    CHECK_NEAR (p[0].beat, 0.0, 0.0);
    CHECK_NEAR (p[0].bpm, 140.0, 0.0);
    CHECK_NEAR (p[1].beat, 4.0, 0.0);
}

TEST_CASE ("TempoMap: invalid tempo rejected and previous map kept")
{
    TempoMap m;
    CHECK (m.setTempoPoints ({ { 0.0, 100.0 } }));
    CHECK (! m.setTempoPoints ({ { 0.0, 0.0 } }));
    CHECK (! m.setTempoPoints ({ { 0.0, -5.0 } }));
    CHECK (! m.setTempoPoints ({}));
    CHECK_NEAR (m.bpmAtBeat (0.0), 100.0, 0.0);
}

TEST_CASE ("TempoMap: time signature changes 4/4 -> 3/4 -> 6/8")
{
    TempoMap m;
    CHECK (m.setTimeSigPoints ({ { 0, 4, 4 }, { 2, 3, 4 }, { 4, 6, 8 } }));
    CHECK_NEAR (m.beatAtBar (2), 8.0, 1e-12);    // 2 bars of 4
    CHECK_NEAR (m.beatAtBar (4), 14.0, 1e-12);   // + 2 bars of 3
    CHECK_NEAR (m.beatAtBar (5), 17.0, 1e-12);   // 6/8 bar = 3 quarter notes

    auto bb = m.barBeatAtBeat (9.5);             // bar 2 (3/4), beat 1, half
    CHECK (bb.bar == 2);
    CHECK (bb.beatInBar == 1);
    CHECK_NEAR (bb.fraction, 0.5, 1e-9);
    CHECK (bb.numerator == 3);

    bb = m.barBeatAtBeat (14.5);                 // first 6/8 bar, eighth index 1
    CHECK (bb.bar == 4);
    CHECK (bb.beatInBar == 1);
    CHECK (bb.denominator == 8);
}

TEST_CASE ("TempoMap: bar boundaries robust to floating point")
{
    TempoMap m;
    CHECK (m.setTempoPoints ({ { 0.0, 133.0 } }));
    for (int bar = 0; bar < 500; ++bar)
    {
        const double beat = m.beatAtSeconds (m.secondsAtBeat (bar * 4.0));
        auto bb = m.barBeatAtBeat (beat);
        if (bb.bar != bar || bb.beatInBar != 0)
        {
            CHECK (bb.bar == bar);
            break;
        }
    }
}

TEST_CASE ("TempoMap: invalid time signatures rejected")
{
    TempoMap m;
    CHECK (! m.setTimeSigPoints ({ { 0, 0, 4 } }));
    CHECK (! m.setTimeSigPoints ({ { 0, 4, 3 } }));
    CHECK (! m.setTimeSigPoints ({ { -1, 4, 4 } }));
    CHECK (m.setTimeSigPoints ({ { 0, 7, 8 } }));
}

TEST_CASE ("TempoMap: offset moves bar 1 to the first downbeat")
{
    TempoMap m;
    CHECK (m.setTempoPoints ({ { 0.0, 120.0 }, { 16.0, 60.0 } }));
    CHECK (m.setOffsetSeconds (0.75));
    CHECK_NEAR (m.secondsAtBeat (0.0), 0.75, 1e-12);
    CHECK_NEAR (m.secondsAtBeat (16.0), 8.75, 1e-12);
    CHECK_NEAR (m.secondsAtBeat (17.0), 9.75, 1e-12);
    CHECK_NEAR (m.beatAtSeconds (9.75), 17.0, 1e-12);
    CHECK_NEAR (m.beatAtSeconds (0.75), 0.0, 1e-12);

    // Before the offset: negative beats at the first tempo, bar -1 in the first signature.
    CHECK_NEAR (m.beatAtSeconds (0.25), -1.0, 1e-12);
    CHECK_NEAR (m.secondsAtBeat (-1.0), 0.25, 1e-12);
    const auto bb = m.barBeatAtBeat (-1.0);
    CHECK (bb.bar == -1);
    CHECK (bb.beatInBar == 3);
    CHECK_NEAR (m.beatAtBar (-1), -4.0, 1e-12);

    for (double s = 0.0; s < 20.0; s += 0.173)
        CHECK_NEAR (m.secondsAtBeat (m.beatAtSeconds (s)), s, 1e-9);

    CHECK (! m.setOffsetSeconds (-0.1));
    CHECK (! m.setOffsetSeconds (std::nan ("")));
    CHECK_NEAR (m.getOffsetSeconds(), 0.75, 0.0);
}
