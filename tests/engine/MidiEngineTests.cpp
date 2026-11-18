// External MIDI scheduling in the engine (docs/04): timing, mute/solo, note recovery, no allocation.

#include "AllocationGuard.h"
#include "TestHarness.h"
#include "engine/AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

using namespace b4nd;

namespace
{
constexpr double sr = 48000.0;
constexpr int block = 256;

struct Rig
{
    Rig()
    {
        for (auto& b : buf) b.assign (block, 0.0f);
        for (int i = 0; i < 2; ++i) ptrs[i] = buf[i].data();
        engine.prepare (sr, block, 2, 0);
        engine.setRouting (OutputRouting::createForDevice (2));
        engine.setLimiterEnabled (false);   // timing checks against the device latency; see the limiter test
        // Offline, callbacks run back to back (microseconds apart): the jitter filter would see
        // a "late device" and relock every time. These tests check the per-callback relation;
        // the filter has its own test below with a simulated clock.
        engine.setClockSmoothing (false);
    }
    void cmd (EngineCommand::Type t, std::int64_t s = 0)
    {
        EngineCommand c;
        c.type = t;
        c.samples = s;
        engine.postCommand (c);
    }
    void run (int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            const auto before = AudioEngine::clockNs();
            engine.process (nullptr, 0, ptrs, 2, block);
            MidiOutEvent e;
            while (engine.popMidi (e))
            {
                got.push_back (e);
                callbackNs.push_back (before);
                callbackPos.push_back (engine.getPosition() - block);
            }
        }
    }
    int count (bool (MidiBytes::*pred)() const noexcept) const
    {
        int n = 0;
        for (const auto& e : got) n += (e.msg.*pred)() ? 1 : 0;
        return n;
    }
    // On the heap: the engine is ~600 kB and Windows threads have a 1 MB stack.
    std::unique_ptr<AudioEngine> enginePtr = std::make_unique<AudioEngine>();
    AudioEngine& engine = *enginePtr;
    std::vector<float> buf[2];
    float* ptrs[2] {};
    std::vector<MidiOutEvent> got;
    std::vector<std::int64_t> callbackNs, callbackPos;
};

std::unique_ptr<PlaybackPlan> planWithNotes (std::vector<MidiSampleEvent> ev, int slot = 0, int port = 1, double earlyMs = 0.0)
{
    auto p = std::make_unique<PlaybackPlan>();
    p->lengthSamples = (std::int64_t) (sr * 30);
    MidiTrackRuntime t;
    t.slot = slot;
    t.port = port;
    t.sendEarlyMs = earlyMs;
    t.events = std::move (ev);
    t.chase = ChaseIndex::build (t.events);
    p->midiTracks.push_back (std::move (t));
    return p;
}

MidiSampleEvent on (std::int64_t s, int note, int ch = 0) { return { s, MidiBytes::make ((std::uint8_t) (0x90 | ch), (std::uint8_t) note, 100) }; }
MidiSampleEvent off (std::int64_t s, int note, int ch = 0) { return { s, MidiBytes::make ((std::uint8_t) (0x80 | ch), (std::uint8_t) note, 0) }; }
} // namespace

TEST_CASE ("MIDI engine: events go out in order, on the right port, timed by sample position")
{
    Rig r;
    r.engine.setOutputLatencyMs (10.0);
    r.engine.setPlan (planWithNotes ({ on (1000, 60), off (24000, 60), on (24000, 62), off (48000, 62) }, 0, 3), sr);
    r.cmd (EngineCommand::Type::Play);
    r.run (200);
    CHECK (r.got.size() == 4);
    if (r.got.size() != 4) return;
    for (auto& e : r.got) CHECK (e.port == 3);
    CHECK (r.got[0].msg.isNoteOn() && r.got[1].msg.isNoteOff() && r.got[2].msg.isNoteOn());
    // dueNs = callback start + offset in block + output latency
    const double expected0 = 1000.0 - (double) r.callbackPos[0];
    const double dueOffsetMs0 = (double) (r.got[0].dueNs - r.callbackNs[0]) / 1e6;
    CHECK (dueOffsetMs0 > expected0 / 48.0 + 10.0 - 0.5 && dueOffsetMs0 < expected0 / 48.0 + 10.0 + 0.5);
    // Every event: due = its callback's start + (sample - block start) / sr + latency.
    // (Offline, callbacks run back to back, so only the per-callback relation is meaningful.)
    const std::int64_t samples[] = { 1000, 24000, 24000, 48000 };
    for (size_t i = 0; i < 4; ++i)
    {
        const double want = (double) (samples[i] - r.callbackPos[i]) / 48.0 + 10.0;
        const double got = (double) (r.got[i].dueNs - r.callbackNs[i]) / 1e6;
        CHECK (got > want - 0.5 && got < want + 0.5);
    }
}

TEST_CASE ("MIDI engine: module latency compensation sends earlier, never before the block")
{
    Rig r;
    r.engine.setOutputLatencyMs (5.0);
    r.engine.setPlan (planWithNotes ({ on (4800, 60), on (100, 61) }, 0, 0, 30.0), sr);   // module 30 ms slow
    r.cmd (EngineCommand::Type::Play);
    r.run (40);
    CHECK (r.got.size() == 2);
    for (size_t i = 0; i < r.got.size(); ++i)
        CHECK (r.got[i].dueNs >= r.callbackNs[i]);   // clamped, never in the past of its own block
}

TEST_CASE ("MIDI engine: STOP, PAUSE, LOCATE and EMERGENCY release every sounding note")
{
    const EngineCommand::Type stops[] = { EngineCommand::Type::Stop, EngineCommand::Type::Pause,
                                          EngineCommand::Type::Locate, EngineCommand::Type::EmergencyStop };
    for (auto st : stops)
    {
        Rig r;
        r.engine.setPlan (planWithNotes ({ on (100, 48), on (200, 52, 2), off (400000, 48), off (400000, 52, 2) }), sr);
        r.cmd (EngineCommand::Type::Play);
        r.run (20);
        CHECK (r.engine.getActiveMidiNotes (1) == 2);
        r.got.clear();
        r.cmd (st, 96000);
        r.run (20);
        int offs = 0, allOff = 0, sustainOff = 0;
        for (auto& e : r.got)
        {
            offs += e.msg.isNoteOff() ? 1 : 0;
            allOff += e.msg.isController (123) ? 1 : 0;
            sustainOff += (e.msg.isController (64) && e.msg.data[2] == 0) ? 1 : 0;
        }
        CHECK (offs == 2);
        CHECK (allOff == 2);       // both used channels
        CHECK (sustainOff == 2);
        if (st == EngineCommand::Type::Locate)
        {
            // Playing on at the new position: the two held notes are chased (started again there).
            int ons = 0;
            for (auto& e : r.got) ons += e.msg.isNoteOn() ? 1 : 0;
            CHECK (ons == 2);
            CHECK (r.engine.getActiveMidiNotes (1) == 2);
        }
        else
        {
            CHECK (r.engine.getActiveMidiNotes (1) == 0);
        }
    }
}

TEST_CASE ("MIDI engine: locate past a note never sends its stray note-off twice")
{
    Rig r;
    r.engine.setPlan (planWithNotes ({ on (100, 60), off (48000, 60), on (96000, 64), off (120000, 64) }), sr);
    r.cmd (EngineCommand::Type::Play);
    r.run (10);
    r.cmd (EngineCommand::Type::Locate, 40000);   // releases 60, then its scheduled note-off at 48000 must be skipped
    r.run (400);
    int offs60 = 0;
    for (auto& e : r.got) offs60 += (e.msg.isNoteOff() && e.msg.data[1] == 60) ? 1 : 0;
    CHECK (offs60 == 1);
    CHECK (r.count (&MidiBytes::isNoteOn) == 2);
}

TEST_CASE ("MIDI engine: mute blocks new notes but lets note-offs through; solo on another track mutes MIDI")
{
    Rig r;
    auto plan = planWithNotes ({ on (100, 60), off (30000, 60), on (40000, 62), off (50000, 62) }, 5);
    TrackRuntime audio;
    audio.slot = 0;
    plan->tracks.push_back (audio);
    r.engine.setPlan (std::move (plan), sr);
    r.cmd (EngineCommand::Type::Play);
    r.run (10);                       // note 60 starts
    r.engine.setTrackMute (5, true);
    r.run (300);
    CHECK (r.count (&MidiBytes::isNoteOn) == 1);    // 62 suppressed
    CHECK (r.count (&MidiBytes::isNoteOff) == 1);   // 60 still released

    Rig s;
    auto p2 = planWithNotes ({ on (100, 60), off (3000, 60) }, 5);
    TrackRuntime a2;
    a2.slot = 0;
    p2->tracks.push_back (a2);
    s.engine.setPlan (std::move (p2), sr);
    s.engine.setTrackSolo (0, true);
    s.cmd (EngineCommand::Type::Play);
    s.run (40);
    CHECK (s.count (&MidiBytes::isNoteOn) == 0);
}

TEST_CASE ("MIDI engine: song change releases notes of the old song")
{
    Rig r;
    r.engine.setPlan (planWithNotes ({ on (100, 60), off (900000, 60) }), sr);
    r.cmd (EngineCommand::Type::Play);
    r.run (10);
    r.engine.setPlan (planWithNotes ({ on (100, 70), off (2000, 70) }), sr);
    r.cmd (EngineCommand::Type::Stop);
    r.cmd (EngineCommand::Type::Locate, 0);   // as AppState::play() does
    r.cmd (EngineCommand::Type::Play);
    r.run (40);
    bool released60 = false, played70 = false;
    for (auto& e : r.got)
    {
        released60 |= e.msg.isNoteOff() && e.msg.data[1] == 60;
        played70 |= e.msg.isNoteOn() && e.msg.data[1] == 70;
    }
    if (! (released60 && played70))
        for (auto& e : r.got) std::printf ("    %02x %d %d\n", e.msg.data[0], e.msg.data[1], e.msg.data[2]);
    CHECK (released60 && played70);
    r.engine.collectGarbage();
}

TEST_CASE ("MIDI engine: dense MIDI, locates and stops never allocate on the audio thread")
{
    Rig r;
    std::vector<MidiSampleEvent> ev;
    for (int i = 0; i < 5000; ++i)
    {
        ev.push_back (on (i * 200, 36 + i % 48, i % 16));
        ev.push_back (off (i * 200 + 150, 36 + i % 48, i % 16));
    }
    r.engine.setPlan (planWithNotes (std::move (ev)), sr);
    std::size_t allocs = 0;
    {
        b4ndtest::AllocationGuard g;
        EngineCommand c;
        c.type = EngineCommand::Type::Play;
        r.engine.postCommand (c);
        MidiOutEvent e;
        for (int i = 0; i < 2000; ++i)
        {
            r.engine.process (nullptr, 0, r.ptrs, 2, block);
            while (r.engine.popMidi (e)) {}
            if (i == 700) { c.type = EngineCommand::Type::Locate; c.samples = 10000; r.engine.postCommand (c); }
            if (i == 1500) { c.type = EngineCommand::Type::EmergencyStop; r.engine.postCommand (c); }
        }
        allocs = g.count();
    }
    CHECK (allocs == 0);
    CHECK (r.engine.getMidiDroppedCount() == 0);
}

namespace
{
// Simulated wall clock for the timing tests below: the device is perfectly regular,
// the callback is observed late by a random amount.
std::int64_t fakeNow = 0;
std::int64_t fakeClock() { return fakeNow; }
} // namespace

TEST_CASE ("MIDI engine: callback jitter does not reach MIDI timing (clock filter)")
{
    Rig r;
    r.engine.setClockSmoothing (true);
    r.engine.setTimeSourceForTesting (&fakeClock);
    r.engine.setOutputLatencyMs (10.0);
    // A note every 100 ms for 20 s.
    std::vector<MidiSampleEvent> ev;
    for (int i = 0; i < 200; ++i)
    {
        ev.push_back (on (4800 * i + 37, 60));
        ev.push_back (off (4800 * i + 2437, 60));
    }
    r.engine.setPlan (planWithNotes (ev), sr);
    std::mt19937 rng (7);
    std::uniform_real_distribution<double> jitter (0.0, 2.0e6);   // up to 2 ms late
    const double blockNs = block * 1.0e9 / sr;
    const double epoch = 1.0e12;
    r.cmd (EngineCommand::Type::Play);
    std::vector<MidiOutEvent> got;
    std::int64_t playStartSample = -1;
    for (int b = 0; b < 4000; ++b)
    {
        fakeNow = (std::int64_t) (epoch + b * blockNs + jitter (rng));
        const auto posBefore = r.engine.getPosition();
        r.engine.process (nullptr, 0, r.ptrs, 2, block);
        if (playStartSample < 0 && r.engine.getPosition() > posBefore)
            playStartSample = (std::int64_t) b * block - posBefore;   // device sample at song position 0
        MidiOutEvent e;
        while (r.engine.popMidi (e))
            if (e.msg.isNoteOn())
                got.push_back (e);
    }
    CHECK (got.size() == 200);
    // Ideal: due = epoch + (device sample of the note) / sr + latency (+ a constant offset the
    // filter cannot know: the average lateness). Compare the spread after settling (10 s).
    double lo = 1e18, hi = -1e18;
    for (size_t i = 100; i < got.size(); ++i)
    {
        const double ideal = epoch + (double) (playStartSample + 4800 * (std::int64_t) i + 37) * 1.0e9 / sr + 10.0e6;
        const double d = (double) got[i].dueNs - ideal;
        lo = std::min (lo, d);
        hi = std::max (hi, d);
    }
    std::printf ("    note timing spread with 2 ms callback jitter: %.3f ms (unfiltered would be ~2 ms)\n", (hi - lo) / 1e6);
    CHECK (hi - lo < 0.7e6);
    CHECK (r.engine.getClockRelockCount() == 0);
}

TEST_CASE ("MIDI engine: a module slower than the output latency gets its notes ahead (lookahead)")
{
    Rig r;
    r.engine.setClockSmoothing (true);
    r.engine.setTimeSourceForTesting (&fakeClock);
    r.engine.setOutputLatencyMs (5.0);
    // Module needs 40 ms: notes must be sent 35 ms before their block is even processed.
    r.engine.setPlan (planWithNotes ({ on (9600, 60), off (14400, 60), on (48000, 62), off (52800, 62) }, 0, 1, 40.0), sr);
    const double blockNs = block * 1.0e9 / sr;
    const double epoch = 2.0e12;
    r.cmd (EngineCommand::Type::Play);
    std::vector<std::pair<MidiOutEvent, std::int64_t>> got;   // event, callback time
    std::int64_t playStartSample = -1;
    for (int b = 0; b < 400; ++b)
    {
        fakeNow = (std::int64_t) (epoch + b * blockNs);
        const auto posBefore = r.engine.getPosition();
        r.engine.process (nullptr, 0, r.ptrs, 2, block);
        if (playStartSample < 0 && r.engine.getPosition() > posBefore)
            playStartSample = (std::int64_t) b * block - posBefore;
        MidiOutEvent e;
        while (r.engine.popMidi (e))
            got.push_back ({ e, fakeNow });
    }
    CHECK (got.size() == 4);
    const std::int64_t samples[] = { 9600, 14400, 48000, 52800 };
    for (size_t i = 0; i < got.size() && i < 4; ++i)
    {
        const double ideal = epoch + (double) (playStartSample + samples[i]) * 1.0e9 / sr + 5.0e6 - 40.0e6;
        CHECK (std::abs ((double) got[i].first.dueNs - ideal) < 0.1e6);   // exact, not clamped late
        CHECK (got[i].first.dueNs >= got[i].second);                      // queued before it is due
    }
}

namespace
{
std::unique_ptr<PlaybackPlan> planWithClock (TempoMap tempo, int port = 2, double earlyMs = 0.0)
{
    auto p = std::make_unique<PlaybackPlan>();
    p->lengthSamples = (std::int64_t) (sr * 60);
    p->tempo = std::move (tempo);
    p->midiClocks.push_back ({ port, earlyMs });
    return p;
}
TempoMap tempoAt (double bpm)
{
    TempoMap t;
    t.setTempoPoints ({ { 0.0, bpm } });
    return t;
}
} // namespace

TEST_CASE ("MIDI clock: Start, 24 clocks per beat at exact intervals, Stop")
{
    Rig r;
    r.engine.setClockSmoothing (true);
    r.engine.setTimeSourceForTesting (&fakeClock);
    r.engine.setPlan (planWithClock (tempoAt (120.0)), sr);
    const double blockNs = block * 1.0e9 / sr;
    r.cmd (EngineCommand::Type::Play);
    std::vector<MidiOutEvent> got;
    for (int b = 0; b < 375; ++b)   // 2.0 s
    {
        fakeNow = (std::int64_t) (3.0e12 + b * blockNs);
        r.engine.process (nullptr, 0, r.ptrs, 2, block);
        MidiOutEvent e;
        while (r.engine.popMidi (e)) got.push_back (e);
    }
    CHECK (! got.empty() && got.front().msg.data[0] == 0xfa && got.front().port == 2);
    std::vector<std::int64_t> ticks;
    for (const auto& e : got)
        if (e.msg.data[0] == 0xf8) ticks.push_back (e.dueNs);
    // 120 BPM = 2 beats/s = 48 clocks/s: ticks at 0 .. 1.979 s
    CHECK (ticks.size() >= 95 && ticks.size() <= 97);
    double worst = 0.0;
    for (size_t i = 1; i < ticks.size(); ++i)
        worst = std::max (worst, std::abs ((double) (ticks[i] - ticks[i - 1]) - 1.0e9 / 48.0));
    CHECK (worst < 25'000.0);   // one sample of rounding
    CHECK (got.size() > 1 && got[1].msg.data[0] == 0xf8 && got[1].dueNs == got[0].dueNs);   // first clock = beat 1

    got.clear();
    r.cmd (EngineCommand::Type::Pause);
    r.run (2);
    for (const auto& e : r.got) got.push_back (e);
    CHECK (std::count_if (got.begin(), got.end(), [] (const MidiOutEvent& e) { return e.msg.data[0] == 0xfc; }) == 1);
    CHECK (std::none_of (got.begin(), got.end(), [] (const MidiOutEvent& e) { return e.msg.data[0] == 0xf8; }));
}

TEST_CASE ("MIDI clock: locate sends Stop, Song Position on the next 16th, Continue, then clocks from there")
{
    Rig r;
    r.engine.setClockSmoothing (false);
    r.engine.setPlan (planWithClock (tempoAt (120.0)), sr);
    r.cmd (EngineCommand::Type::Play);
    r.run (20);
    r.got.clear();
    r.callbackNs.clear();
    r.callbackPos.clear();
    // Locate to 1.01 s = beat 2.02 -> next 16th is beat 2.25 = 16th #9 = clock 54 (at 1.125 s).
    r.cmd (EngineCommand::Type::Locate, (std::int64_t) (1.01 * sr));
    r.run (60);
    const auto& g = r.got;
    size_t i = 0;
    while (i < g.size() && g[i].msg.data[0] != 0xfc) ++i;
    CHECK (i < g.size());
    size_t j = i;
    while (j < g.size() && g[j].msg.data[0] != 0xf2) ++j;
    CHECK (j < g.size());
    if (j + 2 >= g.size()) return;
    CHECK (g[j].msg.data[1] == 9 && g[j].msg.data[2] == 0);
    CHECK (g[j + 1].msg.data[0] == 0xfb);
    CHECK (g[j + 2].msg.data[0] == 0xf8);
    // No clock between the locate and the Continue
    for (size_t k = i; k < j; ++k) CHECK (g[k].msg.data[0] != 0xf8);
    // The first clock is sent in the callback containing sample 1.125 s
    CHECK (std::abs ((double) r.callbackPos[j + 2] - 1.125 * sr) < block);
}

TEST_CASE ("MIDI clock: follows a tempo change and the offset sends it earlier")
{
    TempoMap t;
    t.setTempoPoints ({ { 0.0, 120.0 }, { 4.0, 60.0 } });   // halves at beat 4 (2.0 s)
    Rig r;
    r.engine.setClockSmoothing (true);
    r.engine.setTimeSourceForTesting (&fakeClock);
    r.engine.setOutputLatencyMs (10.0);
    r.engine.setPlan (planWithClock (t, 1, 4.0), sr);
    const double blockNs = block * 1.0e9 / sr;
    const double epoch = 4.0e12;
    r.cmd (EngineCommand::Type::Play);
    std::vector<MidiOutEvent> ticks;
    std::int64_t playStartSample = -1;
    for (int b = 0; b < 750; ++b)   // 4 s
    {
        fakeNow = (std::int64_t) (epoch + b * blockNs);
        const auto before = r.engine.getPosition();
        r.engine.process (nullptr, 0, r.ptrs, 2, block);
        if (playStartSample < 0 && r.engine.getPosition() > before)
            playStartSample = (std::int64_t) b * block - before;
        MidiOutEvent e;
        while (r.engine.popMidi (e)) if (e.msg.data[0] == 0xf8) ticks.push_back (e);
    }
    // 96 clocks in the first 2 s, then 24/s: 96 + 48 = 144 in 4 s
    CHECK (ticks.size() >= 143 && ticks.size() <= 145);
    if (ticks.size() < 110) return;
    CHECK (std::abs ((double) (ticks[50].dueNs - ticks[49].dueNs) - 1.0e9 / 48.0) < 25'000.0);
    CHECK (std::abs ((double) (ticks[105].dueNs - ticks[104].dueNs) - 1.0e9 / 24.0) < 25'000.0);
    // Clock 96 = beat 4 at 2.0 s: due = start + 2.0 s + latency - offset
    const double ideal = epoch + (double) playStartSample * 1.0e9 / sr + 2.0e9 + 10.0e6 - 4.0e6;
    CHECK (std::abs ((double) ticks[96].dueNs - ideal) < 0.1e6);
}

TEST_CASE ("MIDI engine: locate chase sends program / controllers and restarts held notes, not on muted tracks")
{
    for (int muted = 0; muted < 2; ++muted)
    {
        Rig r;
        std::vector<MidiSampleEvent> ev {
            { 0, MidiBytes::make (0xb0, 0, 87) }, { 0, MidiBytes::make (0xc0, 41) },   // bank + program
            { 10, MidiBytes::make (0xb0, 7, 90) },
            on (100, 48), off (240000, 48),     // pad held to 5 s
            on (1000, 60), off (2000, 60),      // short, over
            on (96000, 64), off (100000, 64),
        };
        r.engine.setPlan (planWithNotes (ev), sr);
        r.engine.setTrackMute (0, muted == 1);
        r.cmd (EngineCommand::Type::Locate, 48000);   // 1 s, stopped
        r.cmd (EngineCommand::Type::Play);
        r.run (2);
        const auto& g = r.got;
        CHECK (g.size() >= 3);
        if (g.size() < 3) continue;
        CHECK (g[0].msg.data[0] == 0xb0 && g[0].msg.data[1] == 0 && g[0].msg.data[2] == 87);
        CHECK (g[1].msg.data[0] == 0xc0 && g[1].msg.data[1] == 41);
        CHECK (g[2].msg.data[0] == 0xb0 && g[2].msg.data[1] == 7);
        int ons = 0;
        for (const auto& e : g) ons += e.msg.isNoteOn() ? 1 : 0;
        CHECK (ons == (muted ? 0 : 1));   // only the pad
        // and the pad's own note-off still arrives at 5 s (unmuted)
        r.got.clear();
        r.run (900);
        int offs48 = 0;
        for (const auto& e : r.got) offs48 += (e.msg.isNoteOff() && e.msg.data[1] == 48) ? 1 : 0;
        CHECK (offs48 == (muted ? 0 : 1));
    }
}

TEST_CASE ("MIDI engine: with the PA limiter on, module notes leave 1 ms later to match the PA")
{
    Rig r;
    r.engine.setLimiterEnabled (true);
    r.engine.setOutputLatencyMs (10.0);
    r.engine.setPlan (planWithNotes ({ on (1000, 60), off (24000, 60) }, 0, 3), sr);
    r.cmd (EngineCommand::Type::Play);
    r.run (200);
    CHECK (r.got.size() == 2);
    if (r.got.size() != 2) return;
    const double want = (double) (1000 - r.callbackPos[0]) / 48.0 + 10.0 + 1.0;
    const double got = (double) (r.got[0].dueNs - r.callbackNs[0]) / 1e6;
    CHECK (got > want - 0.3 && got < want + 0.3);
}

TEST_CASE ("MIDI clock: with bar 1 at 0.5 s, Start and the first clock wait for the downbeat")
{
    Rig r;
    r.engine.setClockSmoothing (true);
    r.engine.setTimeSourceForTesting (&fakeClock);
    auto t = tempoAt (120.0);
    CHECK (t.setOffsetSeconds (0.5));
    r.engine.setPlan (planWithClock (t), sr);
    const double blockNs = block * 1.0e9 / sr;
    const double epoch = 3.0e12;
    r.cmd (EngineCommand::Type::Play);
    std::vector<MidiOutEvent> got;
    std::int64_t playStartSample = -1;
    for (int b = 0; b < 375; ++b)   // 2.0 s
    {
        fakeNow = (std::int64_t) (epoch + b * blockNs);
        const auto posBefore = r.engine.getPosition();
        r.engine.process (nullptr, 0, r.ptrs, 2, block);
        if (playStartSample < 0 && r.engine.getPosition() > posBefore)
            playStartSample = (std::int64_t) b * block - posBefore;
        MidiOutEvent e;
        while (r.engine.popMidi (e)) got.push_back (e);
    }
    CHECK (got.size() > 2);
    if (got.size() > 2)
    {
        CHECK (got[0].msg.data[0] == 0xfa);   // Start, not Song Position + Continue
        CHECK (got[1].msg.data[0] == 0xf8);
        const double ideal = epoch + (double) (playStartSample + 24000) * 1.0e9 / sr;
        CHECK (std::abs ((double) got[0].dueNs - ideal) < 0.1e6);
    }
    const auto clocks = std::count_if (got.begin(), got.end(), [] (const MidiOutEvent& e) { return e.msg.data[0] == 0xf8; });
    CHECK (clocks >= 71 && clocks <= 73);   // 1.5 s at 48 clocks/s
}
