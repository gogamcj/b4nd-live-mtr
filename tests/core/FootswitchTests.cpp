#include "TestHarness.h"
#include "core/Footswitch.h"

using namespace b4nd;

namespace
{
MidiBytes cc (int n, int v, int ch = 0) { return MidiBytes::make ((std::uint8_t) (0xb0 | ch), (std::uint8_t) n, (std::uint8_t) v); }
MidiBytes on (int n, int ch = 0) { return MidiBytes::make ((std::uint8_t) (0x90 | ch), (std::uint8_t) n, 100); }
MidiBytes off (int n, int ch = 0) { return MidiBytes::make ((std::uint8_t) (0x80 | ch), (std::uint8_t) n, 0); }
} // namespace

TEST_CASE ("Footswitch: default mapping, one action per press, releases do nothing")
{
    FootswitchMapper m;
    m.setBindings (FootswitchMapper::defaultBindings());
    CHECK (m.handle (cc (64, 127), "FS", 0).action == LiveAction::PlayPause);
    CHECK (m.handle (cc (64, 0), "FS", 100).action == LiveAction::None);
    CHECK (m.handle (cc (65, 127), "FS", 200).action == LiveAction::NextSong);
    CHECK (m.handle (on (64), "FS", 300).action == LiveAction::PreviousSong);
    CHECK (m.handle (off (64), "FS", 400).action == LiveAction::None);
    CHECK (m.handle (cc (67, 127), "FS", 500).action == LiveAction::NextCue);
    CHECK (m.handle (cc (1, 127), "FS", 600).action == LiveAction::None);    // not mapped
    CHECK (m.handle (MidiBytes::make (0xf8), "FS", 700).action == LiveAction::None);   // clock
}

TEST_CASE ("Footswitch: contact bounce within 50 ms is ignored")
{
    FootswitchMapper m;
    m.setBindings (FootswitchMapper::defaultBindings());
    CHECK (m.handle (cc (65, 127), "FS", 1000).action == LiveAction::NextSong);
    CHECK (m.handle (cc (65, 0), "FS", 1010).action == LiveAction::None);
    CHECK (m.handle (cc (65, 127), "FS", 1020).action == LiveAction::None);   // bounce: no second song skip
    CHECK (m.handle (cc (65, 0), "FS", 1200).action == LiveAction::None);
    CHECK (m.handle (cc (65, 127), "FS", 1300).action == LiveAction::NextSong);
    CHECK (m.handle (on (62), "FS", 2000).action == LiveAction::NextSong);
    CHECK (m.handle (on (62), "FS", 2030).action == LiveAction::None);
}

TEST_CASE ("Footswitch: expression pedal sweep is one press, trigger pedals repeat, latching pedals")
{
    FootswitchMapper m;
    m.setBindings (FootswitchMapper::defaultBindings());
    int presses = 0;
    double t = 0;
    for (int v = 0; v <= 127; v += 3, t += 5)   // a sweep up: one press
        presses += m.handle (cc (64, v), "FS", t).action == LiveAction::PlayPause ? 1 : 0;
    for (int v = 127; v >= 0; v -= 3, t += 5)
        presses += m.handle (cc (64, v), "FS", t).action == LiveAction::PlayPause ? 1 : 0;
    CHECK (presses == 1);

    // "trigger" pedal: sends 127 on every press, never 0
    CHECK (m.handle (cc (65, 127), "FS", 5000).action == LiveAction::NextSong);
    CHECK (m.handle (cc (65, 127), "FS", 5600).action == LiveAction::NextSong);

    // latching pedal: 127, then 0 on the next press
    auto b = FootswitchMapper::defaultBindings();
    for (auto& x : b)
        if (! x.note && x.number == 64)
            x.latching = true;
    m.setBindings (b);
    CHECK (m.handle (cc (64, 127), "FS", 0).action == LiveAction::PlayPause);
    CHECK (m.handle (cc (64, 0), "FS", 800).action == LiveAction::PlayPause);
    CHECK (m.handle (cc (64, 0), "FS", 1600).action == LiveAction::None);   // no change
}

TEST_CASE ("Footswitch: emergency stop needs two presses within 500 ms")
{
    FootswitchMapper m;
    m.setBindings (FootswitchMapper::defaultBindings());
    auto r = m.handle (cc (68, 127), "FS", 0);
    CHECK (r.action == LiveAction::None && r.waitingForSecond);
    m.handle (cc (68, 0), "FS", 100);
    CHECK (m.handle (cc (68, 127), "FS", 300).action == LiveAction::EmergencyStop);
    // too slow: two separate first presses
    m.handle (cc (68, 0), "FS", 400);
    CHECK (m.handle (cc (68, 127), "FS", 2000).action == LiveAction::None);
    m.handle (cc (68, 0), "FS", 2100);
    CHECK (m.handle (cc (68, 127), "FS", 2700).action == LiveAction::None);
    m.handle (cc (68, 0), "FS", 2800);
    CHECK (m.handle (cc (68, 127), "FS", 2900).action == LiveAction::EmergencyStop);
    // a bounce is not a second press
    m.handle (cc (68, 0), "FS", 5000);
    CHECK (m.handle (cc (68, 127), "FS", 5010).action == LiveAction::None);
    m.handle (cc (68, 0), "FS", 5015);
    CHECK (m.handle (cc (68, 127), "FS", 5030).action == LiveAction::None);
}

TEST_CASE ("Footswitch: port and channel filters, learn mode, invalid bindings")
{
    FootswitchMapper m;
    FootswitchBinding b;
    b.port = "Pedal";
    b.channel = 2;
    b.number = 20;
    b.action = LiveAction::Restart;
    FootswitchBinding bad;
    bad.number = 200;
    bad.action = LiveAction::Stop;
    m.setBindings ({ b, bad, FootswitchBinding {} });
    CHECK (m.getBindings().size() == 1);
    CHECK (m.handle (cc (20, 127, 2), "Keyboard", 0).action == LiveAction::None);
    CHECK (m.handle (cc (20, 127, 3), "Pedal", 100).action == LiveAction::None);
    CHECK (m.handle (cc (20, 127, 2), "Pedal", 200).action == LiveAction::Restart);

    m.setLearning (true);
    m.handle (cc (20, 0, 2), "Pedal", 300);
    auto r = m.handle (on (40, 5), "Pedal", 400);
    CHECK (r.learned && r.action == LiveAction::None);
    CHECK (r.input.note && r.input.number == 40 && r.input.channel == 5 && r.input.port == "Pedal");
    CHECK (! m.handle (off (40, 5), "Pedal", 500).learned);   // releases are not learned

    CHECK (std::string (liveActionId (LiveAction::EmergencyStop)) == "emergency_stop");
    CHECK (liveActionFromId ("next_cue") == LiveAction::NextCue);
    CHECK (liveActionFromId ("bogus") == LiveAction::None);
    for (int a = 0; a < (int) LiveAction::count; ++a)
        CHECK (liveActionFromId (liveActionId ((LiveAction) a)) == (LiveAction) a);
}
