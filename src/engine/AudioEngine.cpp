#include "engine/AudioEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace b4nd
{

namespace
{
constexpr double twoPi = 6.283185307179586;

std::int64_t nowNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds> (
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void atomicMax (std::atomic<float>& a, float v) noexcept
{
    float cur = a.load (std::memory_order_relaxed);
    while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
}
} // namespace

void AudioEngine::prepare (double sr, int maxBlockSize, int numOutputs, int numInputs)
{
    prepared.store (false, std::memory_order_release);

    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    numOuts = std::clamp (numOutputs, 0, maxOutputs);
    numIns = std::max (0, numInputs);

    xruns.prepare (sampleRate);
    for (auto& p : peaks)
        p.store (0.0f);

    masterGain = 1.0f;
    fadeAction = FadeAction::None;
    fadeStep = (float) (1.0 / std::max (1.0, 0.005 * sampleRate));   // 5 ms
    testPhase = 0.0;
    testClock = 0;

    // A device (re)start never resumes playback or a test by itself.
    transport.store ((int) EngineTransport::Stopped);
    testSignal.store ((int) TestSignal::None);
    testOutput.store (-1);

    prepared.store (true, std::memory_order_release);
}

void AudioEngine::release()
{
    prepared.store (false, std::memory_order_release);
    transport.store ((int) EngineTransport::Stopped);
    testSignal.store ((int) TestSignal::None);
}

bool AudioEngine::postCommand (const EngineCommand& c) noexcept
{
    return commands.push (c);
}

bool AudioEngine::popEvent (EngineEvent& e) noexcept
{
    return events.pop (e);
}

void AudioEngine::setRouting (const OutputRouting& r) noexcept
{
    for (int i = 0; i < maxOutputs; ++i)
    {
        int bus = -1;
        bool pa = false;
        if (i < r.getNumOutputs())
        {
            const auto& o = r.getOutput (i);
            if (o.source)
                bus = (int) o.source->kind;
            pa = o.isPA;
        }
        routes[(size_t) i].bus.store (bus, std::memory_order_relaxed);
        routes[(size_t) i].isPA.store (pa, std::memory_order_relaxed);
    }
}

float AudioEngine::takeOutputPeak (int output) noexcept
{
    if (output < 0 || output >= maxOutputs)
        return 0.0f;
    return peaks[(size_t) output].exchange (0.0f, std::memory_order_relaxed);
}

void AudioEngine::pushEvent (EngineEvent::Type t, float load) noexcept
{
    EngineEvent e;
    e.type = t;
    e.samplePosition = position.load (std::memory_order_relaxed);
    e.load = load;
    events.push (e);   // dropped if the UI is not draining: never block the audio thread
}

void AudioEngine::handleCommand (const EngineCommand& c) noexcept
{
    const auto t = (EngineTransport) transport.load (std::memory_order_relaxed);

    switch (c.type)
    {
        case EngineCommand::Type::Play:
            if (fadeAction == FadeAction::Emergency)
                break;   // finish the emergency fade first; a new PLAY must come after it
            emergencyLatched.store (false, std::memory_order_release);
            testSignal.store ((int) TestSignal::None, std::memory_order_release);
            if (songLength > 0 && position.load (std::memory_order_relaxed) >= songLength)
                position.store (0, std::memory_order_relaxed);
            fadeAction = FadeAction::None;
            masterGain = 1.0f;
            transport.store ((int) EngineTransport::Playing, std::memory_order_release);
            break;

        case EngineCommand::Type::Pause:
            if (t == EngineTransport::Playing)
                transport.store ((int) EngineTransport::Paused, std::memory_order_release);
            break;

        case EngineCommand::Type::Stop:
            if (fadeAction != FadeAction::Emergency)
                fadeAction = FadeAction::Stop;
            break;

        case EngineCommand::Type::Locate:
        {
            auto target = std::max<std::int64_t> (0, c.samples);
            if (songLength > 0)
                target = std::min (target, songLength);
            position.store (target, std::memory_order_relaxed);
        }
            break;

        case EngineCommand::Type::EmergencyStop:
            fadeAction = FadeAction::Emergency;
            emergencyLatched.store (true, std::memory_order_release);
            break;

        case EngineCommand::Type::SetSongLength:
            songLength = std::max<std::int64_t> (0, c.samples);
            break;

        case EngineCommand::Type::StartTest:
            if (t == EngineTransport::Playing || fadeAction != FadeAction::None)
            {
                pushEvent (EngineEvent::Type::CommandRejected);
                break;
            }
            testGain = std::pow (10.0f, std::clamp (c.value, -60.0f, -6.0f) / 20.0f);   // never louder than -6 dBFS
            emergencyLatched.store (false, std::memory_order_release);
            testOutput.store (c.output, std::memory_order_release);
            testPhase = 0.0;
            testClock = 0;
            masterGain = 1.0f;
            testSignal.store (c.intValue, std::memory_order_release);
            break;

        case EngineCommand::Type::StopTest:
            testSignal.store ((int) TestSignal::None, std::memory_order_release);
            testOutput.store (-1, std::memory_order_release);
            break;
    }
}

float AudioEngine::nextNoise() noexcept
{
    noiseState = noiseState * 1664525u + 1013904223u;
    const float white = (float) ((noiseState >> 9) & 0x7fffff) / (float) 0x7fffff * 2.0f - 1.0f;
    // Cheap pink-ish filter (Paul Kellet economy version).
    pinkB0 = 0.99765f * pinkB0 + white * 0.0990460f;
    pinkB1 = 0.96300f * pinkB1 + white * 0.2965164f;
    pinkB2 = 0.57000f * pinkB2 + white * 1.0526913f;
    return (pinkB0 + pinkB1 + pinkB2 + white * 0.1848f) * 0.25f;
}

void AudioEngine::renderTest (float* const* outputs, int numOutputs, int start, int numSamples) noexcept
{
    const auto mode = (TestSignal) testSignal.load (std::memory_order_relaxed);
    if (mode == TestSignal::None)
        return;

    const int target = testOutput.load (std::memory_order_relaxed);
    const double inc = twoPi * 1000.0 / sampleRate;
    const auto sr = (std::int64_t) sampleRate;

    for (int i = start; i < start + numSamples; ++i)
    {
        const std::int64_t t = testClock++;
        const float sine = (float) std::sin (testPhase);
        testPhase += inc;
        if (testPhase > twoPi)
            testPhase -= twoPi;

        switch (mode)
        {
            case TestSignal::Tone:
                if (target >= 0 && target < numOutputs && outputs[target] != nullptr)
                    outputs[target][i] += sine * testGain;
                break;

            case TestSignal::Identify:
            {
                if (target < 0 || target >= numOutputs || outputs[target] == nullptr)
                    break;
                // N beeps (100 ms on / 150 ms off), then 1 s silence.
                const int n = target + 1;
                const std::int64_t beepLen = sr / 10, slot = sr / 4;
                const std::int64_t cycle = n * slot + sr;
                const std::int64_t pos = t % cycle;
                if (pos < n * slot && (pos % slot) < beepLen)
                    outputs[target][i] += sine * testGain;
                break;
            }

            case TestSignal::WalkOutputs:
            {
                if (numOutputs <= 0)
                    break;
                const std::int64_t step = sr * 3 / 2;
                const int out = (int) ((t / step) % numOutputs);
                testOutput.store (out, std::memory_order_relaxed);
                if (outputs[out] != nullptr && (t % step) < step - sr / 4)
                    outputs[out][i] += sine * testGain;
                break;
            }

            case TestSignal::BusCheck:
            {
                // Click: 2 kHz 8 ms ticks at 120 BPM, accent (3 kHz) every 4th.
                const std::int64_t beat = sr / 2;
                const std::int64_t inBeat = t % beat;
                const bool accent = ((t / beat) % 4) == 0;
                float click = 0.0f;
                if (inBeat < sr / 125)
                {
                    const double f = accent ? 3000.0 : 2000.0;
                    const float env = 1.0f - (float) inBeat / (float) (sr / 125);
                    click = (float) std::sin (twoPi * f * (double) inBeat / sampleRate) * env;
                }
                // Guide: 300 ms noise burst once per second.
                const float guide = (t % sr) < (sr * 3 / 10) ? nextNoise() : 0.0f;

                for (int ch = 0; ch < numOutputs; ++ch)
                {
                    if (outputs[ch] == nullptr || routes[(size_t) ch].isPA.load (std::memory_order_relaxed))
                        continue;   // never put test click/guide on a PA output
                    const auto bus = routes[(size_t) ch].bus.load (std::memory_order_relaxed);
                    if (bus == (int) BusKind::Click)
                        outputs[ch][i] += click * testGain;
                    else if (bus == (int) BusKind::Guide)
                        outputs[ch][i] += guide * testGain;
                }
                break;
            }

            case TestSignal::None:
                break;
        }
    }
}

void AudioEngine::process (const float* const*, int, float* const* outputs, int numOutputs, int numSamples) noexcept
{
    const auto startNs = nowNs();
    callbacks.fetch_add (1, std::memory_order_relaxed);

    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);

    if (! prepared.load (std::memory_order_acquire) || numSamples <= 0)
        return;

    numOutputs = std::min (numOutputs, maxOutputs);

    // 1. Commands (bounded per block so a flood cannot starve the audio).
    EngineCommand cmd;
    for (int n = 0; n < 64 && commands.pop (cmd); ++n)
        handleCommand (cmd);

    // 2. Render. Phase 2 has no tracks yet: only test signals. Tracks arrive in Phase 3.
    renderTest (outputs, numOutputs, 0, numSamples);

    // 3. Fades (stop / emergency) applied to every output.
    if (fadeAction != FadeAction::None)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            masterGain = std::max (0.0f, masterGain - fadeStep);
            for (int ch = 0; ch < numOutputs; ++ch)
                if (outputs[ch] != nullptr)
                    outputs[ch][i] *= masterGain;
        }

        if (masterGain <= 0.0f)
        {
            const bool wasEmergency = fadeAction == FadeAction::Emergency;
            fadeAction = FadeAction::None;
            masterGain = 1.0f;
            testSignal.store ((int) TestSignal::None, std::memory_order_release);
            testOutput.store (-1, std::memory_order_release);
            transport.store ((int) EngineTransport::Stopped, std::memory_order_release);
            if (wasEmergency)
                pushEvent (EngineEvent::Type::EmergencyComplete);
            else
                position.store (0, std::memory_order_relaxed);
        }
    }
    else if (emergencyLatched.load (std::memory_order_relaxed)
             && (EngineTransport) transport.load (std::memory_order_relaxed) != EngineTransport::Playing)
    {
        // After an emergency stop nothing is output until an explicit PLAY.
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);
    }

    // 4. Transport.
    if ((EngineTransport) transport.load (std::memory_order_relaxed) == EngineTransport::Playing)
    {
        auto pos = position.load (std::memory_order_relaxed) + numSamples;
        if (songLength > 0 && pos >= songLength)
        {
            pos = songLength;
            transport.store ((int) EngineTransport::Stopped, std::memory_order_release);
            pushEvent (EngineEvent::Type::SongEnded);
        }
        position.store (pos, std::memory_order_release);
    }

    // 5. Meters.
    for (int ch = 0; ch < numOutputs; ++ch)
    {
        if (outputs[ch] == nullptr)
            continue;
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = std::max (peak, std::abs (outputs[ch][i]));
        atomicMax (peaks[(size_t) ch], peak);
    }

    // 6. Timing / dropout detection.
    const auto flags = xruns.onCallback (startNs, nowNs(), numSamples);
    loadTelemetry.store ((float) xruns.getLoad(), std::memory_order_relaxed);
    lateTelemetry.store (xruns.getLateCount(), std::memory_order_relaxed);
    overloadTelemetry.store (xruns.getOverloadCount(), std::memory_order_relaxed);
    if (flags & XrunDetector::LateCallback)
        pushEvent (EngineEvent::Type::LateCallback, (float) xruns.getLoad());
    if (flags & XrunDetector::DspOverload)
        pushEvent (EngineEvent::Type::DspOverload, (float) xruns.getLoad());
}

} // namespace b4nd
