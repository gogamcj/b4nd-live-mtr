// Latency measurement in the engine (docs/04 §4): loopback, module test notes, safety rules.

#include "AllocationGuard.h"
#include "TestHarness.h"
#include "engine/AudioEngine.h"

#include <cmath>
#include <deque>
#include <memory>
#include <vector>

using namespace b4nd;

namespace
{
constexpr double sr = 48000.0;
constexpr int block = 256;

std::int64_t fakeNow = 1'000'000'000'000;
std::int64_t fakeClock() { return fakeNow; }

/** 2 outs (OUT1 = PA, OUT2 = in-ear click, the UR22C split), 2 ins. Each callback's input is
    what the "cable" delivered during the previous callback. */
struct ProbeRig
{
    std::unique_ptr<AudioEngine> enginePtr = std::make_unique<AudioEngine>();
    AudioEngine& engine = *enginePtr;
    std::vector<float> o0 = std::vector<float> (block), o1 = std::vector<float> (block);
    std::vector<float> i0 = std::vector<float> (block, 0.0f), i1 = std::vector<float> (block, 0.0f);
    float* outs[2] { o0.data(), o1.data() };
    const float* ins[2] { i0.data(), i1.data() };
    float maxPA = 0.0f, maxClickOut = 0.0f;

    ProbeRig()
    {
        engine.prepare (sr, block, 2, 2);
        engine.setRouting (OutputRouting::createForDevice (2));
        engine.setClockSmoothing (false);
        engine.setTimeSourceForTesting (&fakeClock);
    }
    void process()
    {
        engine.process (ins, 2, outs, 2, block);
        for (int i = 0; i < block; ++i)
        {
            maxPA = std::max (maxPA, std::abs (o0[(size_t) i]));
            maxClickOut = std::max (maxClickOut, std::abs (o1[(size_t) i]));
        }
        fakeNow += (std::int64_t) (block * 1.0e9 / sr);
    }
    void post (const EngineCommand& c) { engine.postCommand (c); }
};
}

TEST_CASE ("Latency: loopback from the in-ear output measures the round trip, never touches the PA, no allocation")
{
    ProbeRig r;
    std::deque<float> cable (700, 0.0f);   // OUT2 -> IN1 through 700 samples
    r.post (AudioEngine::makeProbeCommand (false, 1, 0));
    std::size_t allocs = 0;
    {
        for (int b = 0; b < 2000 && (b < 2 || r.engine.getProbeStatus().state == LatencyProbe::State::Running); ++b)
        {
            {
                b4ndtest::AllocationGuard g;   // around the callback only (the test's cable allocates)
                r.process();
                allocs += g.count();
            }
            for (int i = 0; i < block; ++i)
            {
                cable.push_back (r.o1[(size_t) i]);
                r.i0[(size_t) i] = cable.front();
                cable.pop_front();
            }
        }
    }
    const auto st = r.engine.getProbeStatus();
    CHECK (st.state == LatencyProbe::State::Done);
    CHECK (st.delaySamples == 700 + block);
    CHECK (st.validPings == 5);
    CHECK (! st.midi);
    CHECK (r.maxPA == 0.0f);          // the tick never reaches the PA
    CHECK (r.maxClickOut > 0.2f);
    CHECK (r.maxClickOut <= 0.25f);   // -12 dBFS
    CHECK (allocs == 0);
}

TEST_CASE ("Latency: refused on a PA output, without input, or while playing; PLAY cancels a run")
{
    {
        ProbeRig r;
        r.post (AudioEngine::makeProbeCommand (false, 0, 0));   // OUT1 is the PA
        for (int b = 0; b < 200; ++b) r.process();
        CHECK (r.engine.getProbeStatus().state == LatencyProbe::State::Failed);
        CHECK (r.engine.getProbeStatus().failure == LatencyProbe::Failure::BadSetup);
        CHECK (r.maxPA == 0.0f);
    }
    {
        ProbeRig r;
        r.post (AudioEngine::makeProbeCommand (false, 1, 5));   // no such input
        for (int b = 0; b < 200; ++b) r.process();
        CHECK (r.engine.getProbeStatus().failure == LatencyProbe::Failure::BadSetup);
        CHECK (r.maxClickOut == 0.0f);
    }
    {
        ProbeRig r;
        EngineCommand play;
        play.type = EngineCommand::Type::Play;
        r.post (play);
        r.process();
        r.post (AudioEngine::makeProbeCommand (false, 1, 0));
        for (int b = 0; b < 200; ++b) r.process();
        CHECK (r.engine.getProbeStatus().failure == LatencyProbe::Failure::BadSetup);
    }
    {
        ProbeRig r;
        r.post (AudioEngine::makeProbeCommand (false, 1, 0));
        for (int b = 0; b < 20; ++b) r.process();
        CHECK (r.engine.getProbeStatus().state == LatencyProbe::State::Running);
        EngineCommand play;
        play.type = EngineCommand::Type::Play;
        r.post (play);
        r.process();
        CHECK (r.engine.getProbeStatus().failure == LatencyProbe::Failure::Cancelled);
        r.maxClickOut = 0.0f;
        for (int b = 0; b < 400; ++b) r.process();
        CHECK (r.maxClickOut == 0.0f);   // no more ticks into the song
    }
}

TEST_CASE ("Latency: module test notes leave on time and the module delay is measured")
{
    ProbeRig r;
    r.engine.setOutputLatencyMs (0.0);
    r.post (AudioEngine::makeProbeCommand (true, -1, 1, 2, 10, 38));
    const std::int64_t t0 = fakeNow;
    const double nsPerSample = 1.0e9 / sr;
    const int moduleDelay = 300;
    std::vector<std::int64_t> soundStarts;   // absolute input sample where the module sounds
    std::vector<MidiOutEvent> sent;
    std::int64_t sample = 0;
    for (int b = 0; b < 4000 && (b < 2 || r.engine.getProbeStatus().state == LatencyProbe::State::Running); ++b)
    {
        r.process();
        // The module plays what arrived; its sound lands in the next callback's input.
        MidiOutEvent e;
        while (r.engine.popMidi (e))
        {
            sent.push_back (e);
            if (e.msg.isNoteOn())
                soundStarts.push_back ((std::int64_t) std::llround ((double) (e.dueNs - t0) / nsPerSample) + moduleDelay);
        }
        for (int i = 0; i < block; ++i)
        {
            float v = 0.0f;
            for (auto s0 : soundStarts)
                if (sample + i >= s0)
                    v += 0.3f * (float) std::exp (-(double) (sample + i - s0) / 2400.0) * (i % 2 == 0 ? 1.0f : -1.0f);
            r.i1[(size_t) i] = v;
        }
        sample += block;
    }
    const auto st = r.engine.getProbeStatus();
    CHECK (st.state == LatencyProbe::State::Done);
    CHECK (st.midi);
    CHECK (std::abs (st.delaySamples - (moduleDelay + block)) <= 1);
    CHECK (r.maxClickOut == 0.0f);   // module mode writes no audio
    CHECK (r.maxPA == 0.0f);
    int ons = 0, offs = 0;
    for (const auto& e : sent)
    {
        CHECK (e.port == 2);
        if (e.msg.isNoteOn()) { ++ons; CHECK (e.msg.data[0] == 0x99); CHECK (e.msg.data[1] == 38); }
        if (e.msg.isNoteOff()) ++offs;
    }
    CHECK (ons == 5);
    CHECK (offs == 5);
}
