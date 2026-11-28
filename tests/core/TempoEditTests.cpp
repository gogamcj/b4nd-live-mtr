#include "TestHarness.h"
#include "core/TempoEdit.h"

using namespace b4nd;

namespace
{
TempoMap analysed()
{
    TempoMap m;
    m.setTempoPoints ({ { 0.0, 70.0 }, { 64.0, 72.0 } });
    m.setTimeSigPoints ({ { 0, 4, 4 } });
    m.setOffsetSeconds (1.25);
    return m;
}
} // namespace

TEST_CASE ("TempoEdit: double tempo keeps the tempo change at the same second")
{
    const auto in = analysed();
    TempoMap out;
    CHECK (tempoEdit::scale (in, 2.0, out));
    CHECK_NEAR (out.bpmAtBeat (0.0), 140.0, 1e-9);
    CHECK (out.getTempoPoints().size() == 2);
    CHECK_NEAR (out.getTempoPoints()[1].beat, 128.0, 1e-9);
    CHECK_NEAR (out.secondsAtBeat (128.0), in.secondsAtBeat (64.0), 1e-9);
    CHECK_NEAR (out.getOffsetSeconds(), 1.25, 1e-12);
    TempoMap back;
    CHECK (tempoEdit::scale (out, 0.5, back));
    CHECK_NEAR (back.bpmAtBeat (100.0), 72.0, 1e-9);
    CHECK (! tempoEdit::scale (in, 10.0, out));
    TempoMap fast;
    fast.setTempoPoints ({ { 0.0, 300.0 } });
    CHECK (! tempoEdit::scale (fast, 2.0, out));   // 600 BPM: refused
}

TEST_CASE ("TempoEdit: 4/4 detected, song is 3/4")
{
    const auto in = analysed();
    TempoMap out;
    CHECK (tempoEdit::timeSignature (in, 3, 4, out));
    const auto bb = out.barBeatAtBeat (3.0);
    CHECK (bb.bar == 1 && bb.beatInBar == 0 && bb.numerator == 3);
    CHECK_NEAR (out.getOffsetSeconds(), 1.25, 1e-12);
    CHECK (out.getTempoPoints().size() == 2);   // the tempo is untouched
    CHECK (tempoEdit::timeSignature (in, 6, 8, out));
    CHECK_NEAR (out.beatAtBar (1), 3.0, 1e-12);   // 6/8: three quarter notes per bar
    CHECK (! tempoEdit::timeSignature (in, 3, 3, out));
    CHECK (! tempoEdit::timeSignature (in, 0, 4, out));
    CHECK (! tempoEdit::timeSignature (in, 17, 4, out));
}

TEST_CASE ("TempoEdit: constant tempo and bar 1 position")
{
    const auto in = analysed();
    TempoMap out;
    CHECK (tempoEdit::constantTempo (in, 96.5, out));
    CHECK (out.getTempoPoints().size() == 1);
    CHECK_NEAR (out.bpmAtBeat (200.0), 96.5, 1e-12);
    CHECK (! tempoEdit::constantTempo (in, 10.0, out));
    CHECK (! tempoEdit::constantTempo (in, std::nan (""), out));

    // +1 beat at 70 BPM: bar 1 moves 60/70 s later.
    CHECK (tempoEdit::shiftBarOne (in, 1.0, out));
    CHECK_NEAR (out.getOffsetSeconds(), 1.25 + 60.0 / 70.0, 1e-9);
    // -2 beats would be before 0 s: one bar (4 beats) later instead.
    CHECK (tempoEdit::shiftBarOne (in, -2.0, out));
    CHECK_NEAR (out.getOffsetSeconds(), 1.25 + 2.0 * 60.0 / 70.0, 1e-9);
    CHECK (tempoEdit::barOneAt (in, 0.0, out));
    CHECK_NEAR (out.getOffsetSeconds(), 0.0, 1e-12);
    CHECK (! tempoEdit::barOneAt (in, -1.0, out));
}

TEST_CASE ("TapTempo: median of the taps, restarts after a pause")
{
    TapTempo t;
    CHECK (t.tap (10.0) == 0.0);
    CHECK (t.tap (10.5) == 0.0);
    CHECK_NEAR (t.tap (11.0), 120.0, 1e-9);
    CHECK_NEAR (t.tap (11.52), 120.0, 1e-9);   // one late tap does not move the median much
    CHECK (t.tap (11.98) > 115.0);
    CHECK (t.tap (20.0) == 0.0);                // pause: start over
    CHECK (t.taps() == 1);
    for (int i = 1; i < 20; ++i)
        t.tap (20.0 + i * 0.6);
    CHECK (t.taps() == TapTempo::maxTaps);
    CHECK_NEAR (t.bpm(), 100.0, 1e-6);
}
