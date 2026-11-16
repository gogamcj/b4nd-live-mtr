// Offline render tests for Phase 3: tracks, buses, click, output mapping (docs/06, docs/10 §1).

#include "AllocationGuard.h"
#include "TestHarness.h"
#include "engine/AudioEngine.h"

#include <memory>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace b4nd;

namespace
{
constexpr double sr = 48000.0;

std::shared_ptr<const AudioData> constantData (int channels, std::int64_t frames, float left, float right = 0.0f)
{
    auto d = std::make_shared<AudioData>();
    d->numChannels = channels;
    d->numFrames = frames;
    d->channels.resize ((size_t) channels);
    for (int c = 0; c < channels; ++c)
        d->channels[(size_t) c].assign ((size_t) frames, c == 0 ? left : right);
    return d;
}

struct Player
{
    explicit Player (int outs = 8, int block = 256, OutputRouting routing = OutputRouting::createDefault (8))
        : numOutputs (outs), blockSize (block), buffers ((size_t) outs), ptrs ((size_t) outs), recorded ((size_t) outs)
    {
        for (int i = 0; i < outs; ++i)
        {
            buffers[(size_t) i].assign ((size_t) 4096, 0.0f);
            ptrs[(size_t) i] = buffers[(size_t) i].data();
        }
        engine.prepare (sr, block, outs, 0);
        engine.setRouting (routing);
    }

    void command (EngineCommand::Type t, std::int64_t samples = 0)
    {
        EngineCommand c;
        c.type = t;
        c.samples = samples;
        engine.postCommand (c);
    }

    void run (int totalSamples, int callbackSize = -1)
    {
        const int cb = callbackSize > 0 ? callbackSize : blockSize;
        for (int done = 0; done < totalSamples; done += cb)
        {
            engine.process (nullptr, 0, ptrs.data(), numOutputs, cb);
            for (int ch = 0; ch < numOutputs; ++ch)
                recorded[(size_t) ch].insert (recorded[(size_t) ch].end(), buffers[(size_t) ch].begin(),
                                              buffers[(size_t) ch].begin() + cb);
        }
    }

    double energy (int ch) const
    {
        double e = 0.0;
        for (float s : recorded[(size_t) ch]) e += (double) s * s;
        return e;
    }

    float peak (int ch) const
    {
        float p = 0.0f;
        for (float s : recorded[(size_t) ch]) p = std::max (p, std::abs (s));
        return p;
    }

    /** Sample indices where the signal rises above a threshold after >= 10 ms of quiet. */
    std::vector<int> onsets (int ch, float threshold = 0.05f) const
    {
        std::vector<int> result;
        int quiet = 100000;
        const auto& r = recorded[(size_t) ch];
        for (int i = 0; i < (int) r.size(); ++i)
        {
            if (std::abs (r[(size_t) i]) > threshold)
            {
                if (quiet >= (int) (0.01 * sr))
                    result.push_back (i);
                quiet = 0;
            }
            else
                ++quiet;
        }
        return result;
    }

    void clear() { for (auto& r : recorded) r.clear(); }

    // On the heap: the engine is ~600 kB and Windows threads have a 1 MB stack.
    std::unique_ptr<AudioEngine> enginePtr = std::make_unique<AudioEngine>();
    AudioEngine& engine = *enginePtr;
    int numOutputs, blockSize;
    std::vector<std::vector<float>> buffers;
    std::vector<float*> ptrs;
    std::vector<std::vector<float>> recorded;
};

std::unique_ptr<PlaybackPlan> planWithTrack (TrackRole role, BusKind bus, std::shared_ptr<const AudioData> data,
                                             std::int64_t start = 0, int slot = 0)
{
    auto p = std::make_unique<PlaybackPlan>();
    p->lengthSamples = (std::int64_t) (sr * 10);
    TrackRuntime t;
    t.slot = slot;
    t.role = role;
    t.bus = bus;
    t.clips.push_back ({ std::move (data), start });
    p->tracks.push_back (std::move (t));
    return p;
}

void addClick (PlaybackPlan& p, double bpm, int slot = 100)
{
    p.tempo.setTempoPoints ({ { 0.0, bpm } });
    p.hasClick = true;
    p.clickSlot = slot;
    p.click = PlaybackPlan::makeDefaultClick (sr);
}
} // namespace

TEST_CASE ("Playback: backing track on Main reaches OUT1/2 only")
{
    Player pl;
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 48000, 0.5f, 0.25f)), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (24000);
    CHECK_NEAR (pl.recorded[0].back(), 0.5f, 1e-4f);
    CHECK_NEAR (pl.recorded[1].back(), 0.25f, 1e-4f);
    for (int ch = 2; ch < 8; ++ch)
        CHECK (pl.energy (ch) == 0.0);   // MR never leaks into click/guide/monitor by default
}

TEST_CASE ("Playback: clip starts sample-accurately")
{
    Player pl;
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (1, 4800, 1.0f), 1000), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (4096);
    const auto& r = pl.recorded[0];
    bool silentBefore = true;
    for (int i = 0; i < 1000; ++i)
        if (r[(size_t) i] != 0.0f) silentBefore = false;
    CHECK (silentBefore);
    CHECK (r[1000] != 0.0f);
    CHECK_NEAR (r[2000], 0.70710678f, 1e-4f);   // mono source, centre pan: -3 dB
}

TEST_CASE ("Playback: click goes to CLICK and in-ear MONITOR outputs, never to PA")
{
    Player pl;
    auto plan = std::make_unique<PlaybackPlan>();
    plan->lengthSamples = (std::int64_t) (sr * 10);
    addClick (*plan, 120.0);
    pl.engine.setPlan (std::move (plan), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (96000);   // 2 s = 4 beats at 120 BPM
    CHECK (pl.energy (0) == 0.0);
    CHECK (pl.energy (1) == 0.0);
    CHECK (pl.energy (2) > 0.0);   // CLICK
    CHECK (pl.energy (3) == 0.0);  // GUIDE
    CHECK (pl.energy (6) > 0.0);   // MONITOR L
    CHECK (pl.energy (7) > 0.0);   // MONITOR R

    const auto on = pl.onsets (2);
    CHECK (on.size() == 4);
    if (on.size() == 4)
    {
        CHECK (on[0] < 48);                         // beat 1 at 0 s (within 1 ms)
        CHECK (std::abs (on[1] - 24000) < 48);      // 0.5 s
        CHECK (std::abs (on[3] - 72000) < 48);      // 1.5 s
    }
    // Accent on beat 1 is louder than beat 2.
    float a = 0.0f, b = 0.0f;
    for (int i = 0; i < 1440; ++i) a = std::max (a, std::abs (pl.recorded[2][(size_t) i]));
    for (int i = 24000; i < 25440; ++i) b = std::max (b, std::abs (pl.recorded[2][(size_t) i]));
    CHECK (a > b * 1.2f);
}

TEST_CASE ("Playback: click follows a tempo change")
{
    Player pl;
    auto plan = std::make_unique<PlaybackPlan>();
    plan->lengthSamples = (std::int64_t) (sr * 10);
    addClick (*plan, 120.0);
    plan->tempo.setTempoPoints ({ { 0.0, 120.0 }, { 4.0, 60.0 } });   // bar 2 at 60 BPM
    pl.engine.setPlan (std::move (plan), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (48000 * 4);
    const auto on = pl.onsets (2);
    // 120 BPM: 0, .5, 1, 1.5 ; then 60 BPM: 2, 3
    CHECK (on.size() == 6);
    if (on.size() == 6)
    {
        CHECK (std::abs (on[4] - 96000) < 48);
        CHECK (std::abs (on[5] - 144000) < 48);
    }
}

TEST_CASE ("Playback: 6/8 accents every 6 eighths")
{
    Player pl;
    auto plan = std::make_unique<PlaybackPlan>();
    plan->lengthSamples = (std::int64_t) (sr * 10);
    addClick (*plan, 60.0);   // quarter = 1 s, eighth = 0.5 s
    plan->tempo.setTimeSigPoints ({ { 0, 6, 8 } });
    pl.engine.setPlan (std::move (plan), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (48000 * 4);
    const auto on = pl.onsets (2);
    CHECK (on.size() == 8);   // 0 .. 3.5 s every 0.5 s
    auto peakAt = [&] (int start) { float p = 0; for (int i = start; i < start + 1440; ++i) p = std::max (p, std::abs (pl.recorded[2][(size_t) i])); return p; };
    CHECK (peakAt (0) > peakAt (24000) * 1.2f);        // bar 1 beat 1 accented
    CHECK (peakAt (144000) > peakAt (120000) * 1.2f);  // bar 2 beat 1 (3 s) accented
}

TEST_CASE ("Playback: guide track goes to GUIDE output and monitor, not PA")
{
    Player pl;
    pl.engine.setPlan (planWithTrack (TrackRole::Guide, BusKind::Main, constantData (1, 48000, 0.3f)), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (8192);
    CHECK (pl.energy (0) == 0.0 && pl.energy (1) == 0.0);
    CHECK_NEAR (pl.recorded[3].back(), 0.3f, 1e-4f);
    CHECK (pl.energy (6) > 0.0);
}

TEST_CASE ("Playback: mute, solo (click is solo-safe), master gain only on Main")
{
    Player pl;
    auto plan = std::make_unique<PlaybackPlan>();
    plan->lengthSamples = (std::int64_t) (sr * 10);
    for (int slot = 0; slot < 2; ++slot)
    {
        TrackRuntime t;
        t.slot = slot;
        t.bus = slot == 0 ? BusKind::Main : BusKind::Extra;
        t.clips.push_back ({ constantData (2, 96000, 0.5f, 0.5f), 0 });
        plan->tracks.push_back (std::move (t));
    }
    addClick (*plan, 120.0);
    pl.engine.setPlan (std::move (plan), sr);
    pl.engine.setTrackSolo (1, true);   // solo the Extra track
    pl.command (EngineCommand::Type::Play);
    pl.run (24000);
    CHECK (pl.energy (0) == 0.0);       // Main track silenced by solo
    CHECK (pl.energy (4) > 0.0);        // Extra L
    CHECK (pl.energy (2) > 0.0);        // click still running

    pl.engine.setTrackSolo (1, false);
    pl.engine.setTrackMute (1, true);
    pl.engine.setMasterGain (0.5f);
    pl.clear();
    pl.run (24000);
    CHECK_NEAR (pl.recorded[0].back(), 0.25f, 1e-4f);   // 0.5 * master 0.5
    CHECK (std::abs (pl.recorded[4].back()) < 1e-6f);   // muted (after ramp)
    CHECK (pl.energy (2) > 0.0);
}

TEST_CASE ("Playback: UR22C 2-output split: OUT1 MR mono to PA, OUT2 click in-ear")
{
    Player pl (2, 256, OutputRouting::createForDevice (2));
    auto plan = planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 96000, 0.6f, 0.2f));
    addClick (*plan, 120.0);
    pl.engine.setPlan (std::move (plan), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (96000);
    // OUT1 = (L + R) / 2 = 0.4 constant, no click on top.
    float maxDev = 0.0f;
    for (size_t i = 4800; i < pl.recorded[0].size(); ++i)
        maxDev = std::max (maxDev, std::abs (pl.recorded[0][i] - 0.4f));
    CHECK (maxDev < 1e-4f);
    CHECK (pl.onsets (1).size() == 4);   // click only on OUT2
}

TEST_CASE ("Playback: in-ear monitor can include some MR")
{
    Player pl;
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 48000, 0.5f, 0.5f)), sr);
    pl.engine.setMonitorMix (1.0f, 1.0f, 0.5f);
    pl.command (EngineCommand::Type::Play);
    pl.run (8192);
    CHECK_NEAR (pl.recorded[6].back(), 0.25f, 1e-4f);
}

TEST_CASE ("Playback: a new plan is only picked up when the transport is stopped")
{
    Player pl;
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 480000, 0.1f, 0.1f)), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (4096);
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 480000, 0.9f, 0.9f)), sr);
    pl.run (4096);
    CHECK_NEAR (pl.recorded[0].back(), 0.1f, 1e-4f);   // still the old song while playing
    CHECK (pl.engine.hasPendingPlan());
    pl.command (EngineCommand::Type::Stop);
    pl.run (4096);
    CHECK (! pl.engine.hasPendingPlan());
    pl.engine.collectGarbage();
    pl.clear();
    pl.command (EngineCommand::Type::Play);
    pl.run (8192);
    CHECK_NEAR (pl.recorded[0].back(), 0.9f, 1e-4f);
}

TEST_CASE ("Playback: next song + immediate PLAY never resumes the old song")
{
    Player pl;
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 480000, 0.1f, 0.1f)), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (4096);
    // Song change: new plan, STOP, LOCATE 0, PLAY all arrive within one block (inside the 5 ms fade).
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 480000, 0.9f, 0.9f)), sr);
    pl.command (EngineCommand::Type::Stop);
    pl.command (EngineCommand::Type::Locate, 0);
    pl.command (EngineCommand::Type::Play);
    pl.clear();
    pl.run (48000);
    float maxOld = 0.0f;
    for (size_t i = 512; i < pl.recorded[0].size(); ++i)   // after the fade-out
        if (std::abs (pl.recorded[0][i] - 0.1f) < 1e-3f) maxOld = 1.0f;
    CHECK (maxOld == 0.0f);                                   // old song never comes back
    CHECK_NEAR (pl.recorded[0].back(), 0.9f, 1e-4f);         // new song playing
    CHECK (pl.engine.getTransport() == EngineTransport::Playing);
    CHECK (pl.engine.getPosition() > 40000 && pl.engine.getPosition() <= 48000);
    pl.engine.collectGarbage();
}

TEST_CASE ("Playback: STOP then PLAY during the fade restarts from the located position")
{
    Player pl;
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 480000, 0.5f, 0.5f)), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (4096);
    pl.command (EngineCommand::Type::Stop);
    pl.command (EngineCommand::Type::Locate, 96000);
    pl.command (EngineCommand::Type::Play);
    pl.run (4096);
    CHECK (pl.engine.getTransport() == EngineTransport::Playing);
    CHECK (pl.engine.getPosition() > 96000 && pl.engine.getPosition() < 96000 + 4096);
}

TEST_CASE ("Playback: plan for a different sample rate stays silent")
{
    Player pl;
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 48000, 0.5f, 0.5f)), 44100.0);
    pl.command (EngineCommand::Type::Play);
    pl.run (4096);
    CHECK (pl.energy (0) == 0.0);
}

TEST_CASE ("Playback: driver blocks larger than the prepared size are split correctly")
{
    Player pl (8, 128);
    pl.engine.setPlan (planWithTrack (TrackRole::Audio, BusKind::Main, constantData (1, 480000, 1.0f), 0), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (4000, 1000);
    CHECK (pl.engine.getPosition() == 4000);
    CHECK_NEAR (pl.recorded[0][3999], 0.70710678f, 1e-4f);
}

TEST_CASE ("Playback: song end stops the transport and the click")
{
    Player pl;
    auto plan = std::make_unique<PlaybackPlan>();
    plan->lengthSamples = 30000;
    addClick (*plan, 120.0);
    pl.engine.setPlan (std::move (plan), sr);
    pl.command (EngineCommand::Type::Play);
    pl.run (96000);
    CHECK (pl.engine.getTransport() == EngineTransport::Stopped);
    CHECK (pl.onsets (2).size() == 2);   // beats at 0 and 24000 are inside the 30000-sample song
    float after = 0.0f;
    for (size_t i = 36000; i < pl.recorded[2].size(); ++i)
        after = std::max (after, std::abs (pl.recorded[2][i]));
    CHECK (after == 0.0f);               // nothing after the song end
}

TEST_CASE ("Playback: rendering tracks, click and plan swaps never allocates")
{
    Player pl (8, 64);
    auto make = [] (float v)
    {
        auto p = planWithTrack (TrackRole::Audio, BusKind::Main, constantData (2, 48000 * 5, v, v));
        TrackRuntime g;
        g.slot = 1;
        g.role = TrackRole::Guide;
        g.clips.push_back ({ constantData (1, 48000 * 5, 0.2f), 0 });
        p->tracks.push_back (std::move (g));
        addClick (*p, 133.0);
        return p;
    };
    auto planA = make (0.3f);
    auto planB = make (0.4f);

    std::size_t allocs = 0;
    {
        // Plans are built (allocated) above, on the "message thread". Only the audio side is guarded here.
        pl.engine.setPlan (std::move (planA), sr);
        b4ndtest::AllocationGuard guard;
        EngineCommand c;
        c.type = EngineCommand::Type::Play;
        pl.engine.postCommand (c);
        for (int i = 0; i < 3000; ++i)
            pl.engine.process (nullptr, 0, pl.ptrs.data(), 8, 64);
        c.type = EngineCommand::Type::Locate;
        c.samples = 100000;
        pl.engine.postCommand (c);
        for (int i = 0; i < 500; ++i)
            pl.engine.process (nullptr, 0, pl.ptrs.data(), 8, 64);
        c.type = EngineCommand::Type::Stop;
        pl.engine.postCommand (c);
        for (int i = 0; i < 10; ++i)
            pl.engine.process (nullptr, 0, pl.ptrs.data(), 8, 64);
        allocs = guard.count();
    }
    pl.engine.setPlan (std::move (planB), sr);
    std::size_t allocsSwap = 0;
    {
        b4ndtest::AllocationGuard guard;
        for (int i = 0; i < 10; ++i)
            pl.engine.process (nullptr, 0, pl.ptrs.data(), 8, 64);   // swap happens here
        allocsSwap = guard.count();
    }
    pl.engine.collectGarbage();
    CHECK (allocs == 0);
    CHECK (allocsSwap == 0);
}
