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
struct Rig
{
    explicit Rig (int outs = 8, int block = 256, double sr = 48000.0)
        : numOutputs (outs), blockSize (block), buffers ((size_t) outs, std::vector<float> ((size_t) block)),
          ptrs ((size_t) outs)
    {
        for (int i = 0; i < outs; ++i)
            ptrs[(size_t) i] = buffers[(size_t) i].data();
        engine.prepare (sr, block, outs, 2);
        engine.setRouting (OutputRouting::createDefault (outs));
        energy.assign ((size_t) outs, 0.0);
    }

    void run (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            engine.process (nullptr, 0, ptrs.data(), numOutputs, blockSize);
            for (int ch = 0; ch < numOutputs; ++ch)
                for (float s : buffers[(size_t) ch])
                {
                    energy[(size_t) ch] += (double) s * s;
                    lastPeak = std::max (lastPeak, std::fabs (s));
                }
        }
    }

    float blockPeak() const
    {
        float p = 0.0f;
        for (const auto& b : buffers)
            for (float s : b)
                p = std::max (p, std::fabs (s));
        return p;
    }

    void clearEnergy() { std::fill (energy.begin(), energy.end(), 0.0); lastPeak = 0.0f; }

    bool hasEvent (EngineEvent::Type t)
    {
        EngineEvent e;
        bool found = false;
        while (engine.popEvent (e))
            if (e.type == t)
                found = true;
        return found;
    }

    void post (EngineCommand::Type t, std::int64_t samples = 0, int intValue = 0, int output = -1, float value = -20.0f)
    {
        EngineCommand c;
        c.type = t;
        c.samples = samples;
        c.intValue = intValue;
        c.output = output;
        c.value = value;
        engine.postCommand (c);
    }

    void startTest (TestSignal s, int output = -1, float db = -20.0f)
    {
        post (EngineCommand::Type::StartTest, 0, (int) s, output, db);
    }

    // On the heap: the engine is ~600 kB and Windows threads have a 1 MB stack.
    std::unique_ptr<AudioEngine> enginePtr = std::make_unique<AudioEngine>();
    AudioEngine& engine = *enginePtr;
    int numOutputs, blockSize;
    std::vector<std::vector<float>> buffers;
    std::vector<float*> ptrs;
    std::vector<double> energy;
    float lastPeak = 0.0f;
};
} // namespace

TEST_CASE ("AudioEngine: silent when idle")
{
    Rig r;
    r.run (50);
    for (double e : r.energy)
        CHECK (e == 0.0);
}

TEST_CASE ("AudioEngine: tone test reaches only the selected output at -20 dBFS")
{
    Rig r;
    r.startTest (TestSignal::Tone, 4);
    r.run (100);
    for (int ch = 0; ch < 8; ++ch)
        CHECK ((ch == 4) == (r.energy[(size_t) ch] > 0.0));
    CHECK_NEAR (r.lastPeak, 0.1f, 0.002f);
    CHECK_NEAR (r.engine.takeOutputPeak (4), 0.1f, 0.002f);
    CHECK (r.engine.takeOutputPeak (4) == 0.0f);   // consumed
}

TEST_CASE ("AudioEngine: test level can never exceed -6 dBFS")
{
    Rig r;
    r.startTest (TestSignal::Tone, 0, +12.0f);
    r.run (50);
    CHECK (r.lastPeak <= 0.502f);
}

TEST_CASE ("AudioEngine: identify plays N beeps on output N")
{
    Rig r (8, 240, 48000.0);
    r.startTest (TestSignal::Identify, 2);   // OUT 3 -> 3 beeps per cycle
    const int cycleSamples = 3 * 12000 + 48000;
    int onsets = 0;
    bool wasOn = false;
    for (int b = 0; b < cycleSamples / 240; ++b)
    {
        r.run (1);
        const bool on = r.blockPeak() > 0.01f;
        if (on && ! wasOn) ++onsets;
        wasOn = on;
    }
    CHECK (onsets == 3);
}

TEST_CASE ("AudioEngine: bus check puts click/guide only on in-ear outputs, never on PA")
{
    Rig r;
    r.startTest (TestSignal::BusCheck);
    r.run (400);   // ~2 s
    CHECK (r.energy[0] == 0.0);   // MAIN L (PA)
    CHECK (r.energy[1] == 0.0);   // MAIN R (PA)
    CHECK (r.energy[2] > 0.0);    // CLICK
    CHECK (r.energy[3] > 0.0);    // GUIDE
    for (int ch = 4; ch < 8; ++ch)
        CHECK (r.energy[(size_t) ch] == 0.0);
}

TEST_CASE ("AudioEngine: output test is refused while playing")
{
    Rig r;
    r.post (EngineCommand::Type::Play);
    r.run (2);
    r.startTest (TestSignal::Tone, 0);
    r.run (2);
    CHECK (r.hasEvent (EngineEvent::Type::CommandRejected));
    CHECK (r.engine.getTestSignal() == TestSignal::None);
    CHECK (r.energy[0] == 0.0);
}

TEST_CASE ("AudioEngine: emergency stop silences all outputs within 5 ms + one block")
{
    Rig r;
    r.startTest (TestSignal::Tone, 0);
    r.run (20);
    r.post (EngineCommand::Type::EmergencyStop);
    r.run (1);   // fade happens inside this block (240 samples = 5 ms at 48k, block 256)
    r.clearEnergy();
    r.run (1);
    CHECK (r.blockPeak() == 0.0f);
    CHECK (r.hasEvent (EngineEvent::Type::EmergencyComplete));
    CHECK (r.engine.isEmergencyStopped());
    CHECK (r.engine.getTransport() == EngineTransport::Stopped);
    r.run (100);
    CHECK (r.energy[0] == 0.0);

    r.post (EngineCommand::Type::Play);
    r.run (1);
    CHECK (! r.engine.isEmergencyStopped());
    CHECK (r.engine.getTransport() == EngineTransport::Playing);
}

TEST_CASE ("AudioEngine: emergency fade is monotonic (no click on stop)")
{
    Rig r (1, 1024);
    r.startTest (TestSignal::Tone, 0, -6.0f);
    r.run (4);
    r.post (EngineCommand::Type::EmergencyStop);
    r.run (1);
    // Envelope of the faded block must never rise above the previous local peak.
    float prevPeak = 1.0f;
    bool monotonic = true;
    const auto& b = r.buffers[0];
    for (size_t i = 0; i + 48 <= 240; i += 48)   // ~1 ms windows (one 1 kHz period)
    {
        float p = 0.0f;
        for (size_t k = i; k < i + 48; ++k) p = std::max (p, std::fabs (b[k]));
        if (p > prevPeak + 1e-4f) monotonic = false;
        prevPeak = p;
    }
    CHECK (monotonic);
}

TEST_CASE ("AudioEngine: transport play / pause / stop / song end")
{
    Rig r;
    r.post (EngineCommand::Type::SetSongLength, 48000);
    r.post (EngineCommand::Type::Play);
    r.run (10);
    CHECK (r.engine.getPosition() == 2560);
    r.post (EngineCommand::Type::Pause);
    r.run (10);
    CHECK (r.engine.getPosition() == 2560);
    CHECK (r.engine.getTransport() == EngineTransport::Paused);
    r.post (EngineCommand::Type::Locate, 1000);
    r.run (1);
    CHECK (r.engine.getPosition() == 1000);
    r.post (EngineCommand::Type::Play);
    r.run (500);
    CHECK (r.engine.getPosition() == 48000);
    CHECK (r.engine.getTransport() == EngineTransport::Stopped);
    CHECK (r.hasEvent (EngineEvent::Type::SongEnded));

    r.post (EngineCommand::Type::Play);   // play after end restarts from 0
    r.run (1);
    CHECK (r.engine.getPosition() == 256);
    r.post (EngineCommand::Type::Stop);
    r.run (2);
    CHECK (r.engine.getTransport() == EngineTransport::Stopped);
    CHECK (r.engine.getPosition() == 0);
}

TEST_CASE ("AudioEngine: re-prepare (device restart) never resumes playback")
{
    Rig r;
    r.post (EngineCommand::Type::Play);
    r.run (2);
    r.engine.prepare (96000.0, 128, 8, 2);
    CHECK (r.engine.getTransport() == EngineTransport::Stopped);
}

TEST_CASE ("AudioEngine: process() performs zero heap allocations")
{
    Rig r (8, 64);
    std::size_t allocs = 0;
    {
        b4ndtest::AllocationGuard guard;
        const TestSignal modes[] = { TestSignal::Tone, TestSignal::Identify, TestSignal::BusCheck, TestSignal::WalkOutputs };
        for (int round = 0; round < 4; ++round)
        {
            r.engine.process (nullptr, 0, r.ptrs.data(), 8, 64);
            EngineCommand c;
            c.type = EngineCommand::Type::StartTest;
            c.intValue = (int) modes[round];
            c.output = round;
            c.value = -20.0f;
            r.engine.postCommand (c);
            for (int i = 0; i < 3000; ++i)
                r.engine.process (nullptr, 0, r.ptrs.data(), 8, 64);
            c.type = EngineCommand::Type::StopTest;
            r.engine.postCommand (c);
        }
        EngineCommand c;
        for (auto t : { EngineCommand::Type::Play, EngineCommand::Type::Pause, EngineCommand::Type::Locate,
                        EngineCommand::Type::Play, EngineCommand::Type::EmergencyStop, EngineCommand::Type::Play,
                        EngineCommand::Type::Stop })
        {
            c.type = t;
            r.engine.postCommand (c);
            for (int i = 0; i < 100; ++i)
                r.engine.process (nullptr, 0, r.ptrs.data(), 8, 64);
        }
        allocs = guard.count();
    }
    CHECK (allocs == 0);
}

TEST_CASE ("AllocationGuard: detects a heap allocation (guards the test above)")
{
    std::size_t n = 0;
    {
        b4ndtest::AllocationGuard guard;
        std::vector<int>* volatile v = new std::vector<int> (100);
        delete v;
        n = guard.count();
    }
    CHECK (n >= 2);
}
