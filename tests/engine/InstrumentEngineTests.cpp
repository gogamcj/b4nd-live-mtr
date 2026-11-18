// Software instrument tracks in the engine (docs/05 §4): MIDI to the instrument at the right
// sample, its audio on the track's bus, note recovery, mute/solo, failure isolation, no allocation.

#include "AllocationGuard.h"
#include "TestHarness.h"
#include "engine/AudioEngine.h"

#include <algorithm>
#include <memory>
#include <vector>

using namespace b4nd;

namespace
{
constexpr double sr = 48000.0;
constexpr int block = 256;

/** Outputs 0.1 per sounding note on both channels; records what it received. */
struct FakeInstrument final : InstrumentProcessor
{
    struct Got { std::int64_t blockIndex; int offset; MidiBytes msg; };

    bool render (float* l, float* r, int n, const MidiBytes* msgs, const int* offsets, int count) noexcept override
    {
        if (fail)
            return false;
        int k = 0;
        for (int i = 0; i < n; ++i)
        {
            while (k < count && offsets[k] <= i)
            {
                const auto& m = msgs[k];
                if (got.size() < got.capacity())
                    got.push_back ({ blocks, offsets[k], m });
                if (m.isNoteOn()) ++sounding;
                else if (m.isNoteOff() && sounding > 0) --sounding;
                else if (m.isController (123)) sounding = 0;
                ++k;
            }
            l[i] = r[i] = 0.1f * (float) sounding;
        }
        maxN = std::max (maxN, n);
        ++blocks;
        return true;
    }
    FakeInstrument() { got.reserve (100000); }
    std::vector<Got> got;
    int sounding = 0, maxN = 0;
    std::int64_t blocks = 0;
    bool fail = false;

    int countIf (bool (MidiBytes::*pred)() const noexcept) const
    {
        int c = 0;
        for (const auto& g : got) c += (g.msg.*pred)() ? 1 : 0;
        return c;
    }
};

struct Rig
{
    Rig()
    {
        for (auto& b : buf) b.assign (block, 0.0f);
        for (int i = 0; i < 8; ++i) ptrs[i] = buf[i].data();
        engine.prepare (sr, block, 8, 0);
        engine.setRouting (OutputRouting::createForDevice (8));
    }
    void cmd (EngineCommand::Type t, std::int64_t s = 0)
    {
        EngineCommand c;
        c.type = t;
        c.samples = s;
        engine.postCommand (c);
    }
    /** Runs blocks; returns the peak on OUT1 (Main L). */
    float run (int blocks)
    {
        float pk = 0.0f;
        for (int i = 0; i < blocks; ++i)
        {
            engine.process (nullptr, 0, ptrs, 8, block);
            for (float v : buf[0]) pk = std::max (pk, std::abs (v));
        }
        return pk;
    }
    // On the heap: the engine is ~600 kB and Windows threads have a 1 MB stack.
    std::unique_ptr<AudioEngine> enginePtr = std::make_unique<AudioEngine>();
    AudioEngine& engine = *enginePtr;
    std::vector<float> buf[8];
    float* ptrs[8] {};
};

MidiSampleEvent on (std::int64_t s, int note) { return { s, MidiBytes::make (0x90, (std::uint8_t) note, 100) }; }
MidiSampleEvent off (std::int64_t s, int note) { return { s, MidiBytes::make (0x80, (std::uint8_t) note, 0) }; }

std::unique_ptr<PlaybackPlan> planWith (std::shared_ptr<FakeInstrument> inst, std::vector<MidiSampleEvent> ev, int slot = 3,
                                        BusKind bus = BusKind::Main)
{
    auto p = std::make_unique<PlaybackPlan>();
    p->lengthSamples = (std::int64_t) (sr * 30);
    InstrumentTrackRuntime t;
    t.slot = slot;
    t.bus = bus;
    t.instrument = std::move (inst);
    t.events = std::move (ev);
    t.chase = ChaseIndex::build (t.events);
    p->instrumentTracks.push_back (std::move (t));
    return p;
}
} // namespace

TEST_CASE ("Instruments: MIDI reaches the instrument at the exact sample, audio goes to its bus only")
{
    Rig r;
    auto inst = std::make_shared<FakeInstrument>();
    r.engine.setPlan (planWith (inst, { on (1000, 60), off (24000, 60) }), sr);
    r.cmd (EngineCommand::Type::Play);
    const float pk = r.run (100);
    CHECK (inst->got.size() == 2);
    if (inst->got.size() == 2)
    {
        CHECK (inst->got[0].blockIndex * block + inst->got[0].offset == 1000);
        CHECK (inst->got[1].blockIndex * block + inst->got[1].offset == 24000);
    }
    CHECK (pk > 0.09f && pk < 0.11f);   // Main L (OUT1) carries the instrument
    CHECK (inst->maxN <= block);

    // Same instrument on the Extra bus: nothing on the PA outputs.
    Rig e;
    auto inst2 = std::make_shared<FakeInstrument>();
    e.engine.setPlan (planWith (inst2, { on (100, 60), off (24000, 60) }, 3, BusKind::Extra), sr);
    e.cmd (EngineCommand::Type::Play);
    CHECK (e.run (20) == 0.0f);
}

TEST_CASE ("Instruments: STOP, PAUSE, LOCATE, EMERGENCY and song change release every note")
{
    const EngineCommand::Type stops[] = { EngineCommand::Type::Stop, EngineCommand::Type::Pause,
                                          EngineCommand::Type::Locate, EngineCommand::Type::EmergencyStop };
    for (auto st : stops)
    {
        Rig r;
        auto inst = std::make_shared<FakeInstrument>();
        r.engine.setPlan (planWith (inst, { on (100, 48), on (200, 52), off (900000, 48), off (900000, 52) }), sr);
        r.cmd (EngineCommand::Type::Play);
        r.run (10);
        CHECK (inst->sounding == 2);
        CHECK (r.engine.getActiveInstrumentNotes (0) == 2);
        r.cmd (st, 96000);
        r.run (10);
        CHECK (inst->countIf (&MidiBytes::isNoteOff) == 2);
        // After a locate it plays on: the two held notes are chased at the new position.
        const int expected = st == EngineCommand::Type::Locate ? 2 : 0;
        CHECK (inst->sounding == expected);
        CHECK (r.engine.getActiveInstrumentNotes (0) == expected);
    }

    Rig s;
    auto old = std::make_shared<FakeInstrument>();
    s.engine.setPlan (planWith (old, { on (100, 60), off (900000, 60) }), sr);
    s.cmd (EngineCommand::Type::Play);
    s.run (10);
    s.engine.setPlan (planWith (std::make_shared<FakeInstrument>(), {}), sr);
    s.cmd (EngineCommand::Type::Stop);
    s.cmd (EngineCommand::Type::Locate, 0);
    s.cmd (EngineCommand::Type::Play);
    s.run (20);
    CHECK (old->sounding == 0);
}

TEST_CASE ("Instruments: mute stops new notes and the sound, solo elsewhere silences, release still arrives")
{
    Rig r;
    auto inst = std::make_shared<FakeInstrument>();
    r.engine.setPlan (planWith (inst, { on (100, 60), off (30000, 60), on (40000, 62), off (50000, 62) }), sr);
    r.cmd (EngineCommand::Type::Play);
    r.run (10);
    r.engine.setTrackMute (3, true);
    r.run (2);                          // ramp to zero
    CHECK (r.run (300) == 0.0f);
    CHECK (inst->countIf (&MidiBytes::isNoteOn) == 1);
    CHECK (inst->countIf (&MidiBytes::isNoteOff) == 1);

    Rig s;
    auto i2 = std::make_shared<FakeInstrument>();
    auto plan = planWith (i2, { on (100, 60), off (90000, 60) });
    TrackRuntime audio;
    audio.slot = 0;
    plan->tracks.push_back (audio);
    s.engine.setPlan (std::move (plan), sr);
    s.engine.setTrackSolo (0, true);
    s.cmd (EngineCommand::Type::Play);
    CHECK (s.run (40) == 0.0f);
}

TEST_CASE ("Instruments: a failed instrument is silent and does not affect the others")
{
    Rig r;
    auto bad = std::make_shared<FakeInstrument>();
    auto good = std::make_shared<FakeInstrument>();
    bad->fail = true;
    auto plan = planWith (bad, { on (100, 60), off (90000, 60) }, 2);
    InstrumentTrackRuntime t;
    t.slot = 4;
    t.instrument = good;
    t.events = { on (100, 64), off (90000, 64) };
    plan->instrumentTracks.push_back (t);
    r.engine.setPlan (std::move (plan), sr);
    r.cmd (EngineCommand::Type::Play);
    const float pk = r.run (20);
    CHECK (pk > 0.09f && pk < 0.11f);   // only the good one
    CHECK (good->sounding == 1);
}

TEST_CASE ("Instruments: dense MIDI and mixing never allocate on the audio thread")
{
    Rig r;
    auto inst = std::make_shared<FakeInstrument>();
    std::vector<MidiSampleEvent> ev;
    for (int i = 0; i < 20000; ++i)
    {
        ev.push_back (on (i * 40, 36 + i % 60));
        ev.push_back (off (i * 40 + 30, 36 + i % 60));
    }
    r.engine.setPlan (planWith (inst, std::move (ev)), sr);
    r.cmd (EngineCommand::Type::Play);
    r.run (2);
    b4ndtest::AllocationGuard guard;
    r.run (200);
    r.cmd (EngineCommand::Type::Locate, 48000);
    r.run (20);
    r.cmd (EngineCommand::Type::Stop);
    r.run (20);
    CHECK (guard.count() == 0);
    CHECK (r.engine.getInstrumentEventsDropped() == 0);
}

TEST_CASE ("Instruments: locate chase gives the instrument its program and held notes at the jump")
{
    Rig r;
    auto inst = std::make_shared<FakeInstrument>();
    r.engine.setPlan (planWith (inst, { { 0, MidiBytes::make (0xc0, 5) }, on (100, 48), off (240000, 48), on (1000, 60), off (2000, 60) }), sr);
    r.cmd (EngineCommand::Type::Locate, 96000);
    r.cmd (EngineCommand::Type::Play);
    const float peak = r.run (4);
    CHECK (inst->got.size() >= 2);
    if (inst->got.size() < 2) return;
    CHECK (inst->got[0].msg.data[0] == 0xc0 && inst->got[0].offset == 0);
    CHECK (inst->got[1].msg.isNoteOn() && inst->got[1].msg.data[1] == 48 && inst->got[1].offset == 0);
    CHECK (inst->countIf (&MidiBytes::isNoteOn) == 1);
    CHECK (peak > 0.05f);   // the pad sounds right away
}

TEST_CASE ("Instruments: PDC feeds a look-ahead instrument its MIDI early by its latency")
{
    struct LatentInstrument final : InstrumentProcessor
    {
        bool render (float* l, float* r, int n, const MidiBytes* m, const int* o, int count) noexcept override
        {
            for (int i = 0; i < count; ++i)
                if (got.size() < got.capacity())
                    got.push_back (blocks * 256 + o[i]);
            (void) m;
            std::fill (l, l + n, 0.0f);
            std::fill (r, r + n, 0.0f);
            ++blocks;
            return true;
        }
        int getLatencySamples() const noexcept override { return 480; }
        LatentInstrument() { got.reserve (100); }
        std::vector<std::int64_t> got;   // engine sample (block start + offset) where each event was fed
        std::int64_t blocks = 0;
    };
    Rig r;
    auto inst = std::make_shared<LatentInstrument>();
    auto plan = std::make_unique<PlaybackPlan>();
    plan->lengthSamples = (std::int64_t) (sr * 30);
    InstrumentTrackRuntime t;
    t.slot = 3;
    t.instrument = inst;
    t.events = { on (10000, 60), off (20000, 60), on (100, 62), off (300, 62) };
    std::sort (t.events.begin(), t.events.end(), [] (const MidiSampleEvent& a, const MidiSampleEvent& b) { return a.sample < b.sample; });
    t.chase = ChaseIndex::build (t.events);
    plan->instrumentTracks.push_back (std::move (t));
    r.engine.setPlan (std::move (plan), sr);
    r.cmd (EngineCommand::Type::Play);
    r.run (100);
    // The engine starts playing at its first block; song sample s is block-relative s.
    CHECK (inst->got.size() == 4);
    if (inst->got.size() != 4) return;
    CHECK (inst->got[0] == 0);              // 100 - 480 < 0: as early as possible (first block)
    CHECK (inst->got[1] == 0);              // 300 - 480 < 0
    CHECK (inst->got[2] == 10000 - 480);
    CHECK (inst->got[3] == 20000 - 480);
}
