// External MIDI scheduling in the engine (docs/04): timing, mute/solo, note recovery, no allocation.

#include "AllocationGuard.h"
#include "TestHarness.h"
#include "engine/AudioEngine.h"

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
    AudioEngine engine;
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
        CHECK (r.engine.getActiveMidiNotes (1) == 0);
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
