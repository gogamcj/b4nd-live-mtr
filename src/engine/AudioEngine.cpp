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

std::int64_t AudioEngine::clockNs() noexcept
{
    return nowNs();
}

void AudioEngine::prepare (double sr, int maxBlockSize, int numOutputs, int numInputs)
{
    prepared.store (false, std::memory_order_release);

    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    numOuts = std::clamp (numOutputs, 0, maxOutputs);
    numIns = std::max (0, numInputs);

    xruns.prepare (sampleRate, 1.5, 0.8, 1'000'000, 0.25);   // 250 ms start-up warm-up
    sampleClock.prepare (sampleRate);
    for (auto& p : peaks)
        p.store (0.0f);

    for (auto& bus : busBuffers)
        for (auto& ch : bus)
            ch.assign ((size_t) maxBlock, 0.0f);
    smoothL.fill (0.0f);
    smoothR.fill (0.0f);
    instL.assign ((size_t) maxBlock, 0.0f);
    instR.assign ((size_t) maxBlock, 0.0f);
    instExpectedPos = -1;
    clickVoicePos = -1;
    lastRenderEnd = -1;
    currentPlanValid = currentPlan != nullptr && std::abs (currentPlanRate - sampleRate) < 0.5;

    fadeGain = 1.0f;
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
        routes[(size_t) i].channel.store (i < r.getNumOutputs() && r.getOutput (i).source ? r.getOutput (i).source->channel : 0,
                                          std::memory_order_relaxed);
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
            if (fadeAction == FadeAction::Stop)
            {
                // e.g. next song + PLAY in quick succession: never resume the old song.
                playAfterStop = true;
                playAfterStopPosition = position.load (std::memory_order_relaxed);
                break;
            }
            emergencyLatched.store (false, std::memory_order_release);
            testSignal.store ((int) TestSignal::None, std::memory_order_release);
            probe.cancel();   // a measurement never plays into a song
            if (songLength > 0 && position.load (std::memory_order_relaxed) >= songLength)
                position.store (0, std::memory_order_relaxed);
            fadeAction = FadeAction::None;
            fadeGain = 1.0f;
            transport.store ((int) EngineTransport::Playing, std::memory_order_release);
            break;

        case EngineCommand::Type::Pause:
            if (t == EngineTransport::Playing)
            {
                transport.store ((int) EngineTransport::Paused, std::memory_order_release);
                releaseMidiNotes();
            }
            break;

        case EngineCommand::Type::Stop:
            playAfterStop = false;
            if (fadeAction != FadeAction::Emergency)
                fadeAction = FadeAction::Stop;
            break;

        case EngineCommand::Type::Locate:
        {
            auto target = std::max<std::int64_t> (0, c.samples);
            if (songLength > 0)
                target = std::min (target, songLength);
            position.store (target, std::memory_order_relaxed);
            if (playAfterStop)
                playAfterStopPosition = target;
            releaseMidiNotes();      // a jump never leaves notes hanging
            probe.cancel();
            midiExpectedPos = -1;
            instExpectedPos = -1;
        }
            break;

        case EngineCommand::Type::EmergencyStop:
            playAfterStop = false;
            releaseMidiNotes();      // modules go silent immediately, before the audio fade
            probe.cancel();
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
            probe.cancel();
            testGain = std::pow (10.0f, std::clamp (c.value, -60.0f, -6.0f) / 20.0f);   // never louder than -6 dBFS
            emergencyLatched.store (false, std::memory_order_release);
            testOutput.store (c.output, std::memory_order_release);
            testPhase = 0.0;
            testClock = 0;
            fadeGain = 1.0f;
            testSignal.store (c.intValue, std::memory_order_release);
            break;

        case EngineCommand::Type::StopTest:
            testSignal.store ((int) TestSignal::None, std::memory_order_release);
            testOutput.store (-1, std::memory_order_release);
            break;

        case EngineCommand::Type::StartProbe:
            startProbe (c);
            break;

        case EngineCommand::Type::StopProbe:
            probe.cancel();
            publishProbe();
            break;
    }
}

EngineCommand AudioEngine::makeProbeCommand (bool midi, int output, int input, int port, int channel, int note, int pings)
{
    EngineCommand c;
    c.type = EngineCommand::Type::StartProbe;
    c.output = output;
    c.samples = input;
    c.intValue = (midi ? 1 : 0) | ((port & 0xff) << 8) | ((channel & 0x1f) << 16) | ((note & 0x7f) << 24);
    c.value = (float) pings;
    return c;
}

void AudioEngine::startProbe (const EngineCommand& c) noexcept
{
    probeMidi = (c.intValue & 1) != 0;
    probePort = (c.intValue >> 8) & 0xff;
    probeChannel = std::clamp ((c.intValue >> 16) & 0x1f, 1, 16);
    probeNote = (c.intValue >> 24) & 0x7f;
    probeIn = (int) c.samples;
    probeOut = c.output;

    LatencyProbe::Settings s;
    s.sampleRate = sampleRate;
    s.pings = (int) c.value;
    s.writeBurst = ! probeMidi;
    s.level = 0.25f;                          // -12 dBFS, a short tick
    s.maxSpreadSec = probeMidi ? 0.003 : 0.001;   // modules jitter; a cable does not
    probe.start (s);
    probeRun.fetch_add (1, std::memory_order_relaxed);

    const auto t = (EngineTransport) transport.load (std::memory_order_relaxed);
    const bool busy = t == EngineTransport::Playing || fadeAction != FadeAction::None
                      || testSignal.load (std::memory_order_relaxed) != (int) TestSignal::None
                      || emergencyLatched.load (std::memory_order_relaxed);
    const bool badInput = probeIn < 0 || probeIn >= numIns;
    const bool badOutput = ! probeMidi
                           && (probeOut < 0 || probeOut >= std::min (numOuts, maxOutputs)
                               || routes[(size_t) probeOut].isPA.load (std::memory_order_relaxed));   // never tick the PA
    const bool badPort = probeMidi && (probePort < 0 || probePort >= maxMidiPorts);
    if (busy || badInput || badOutput || badPort)
    {
        probe.abort (LatencyProbe::Failure::BadSetup);
        publishProbe();
        pushEvent (EngineEvent::Type::CommandRejected);
        return;
    }
    publishProbe();
}

void AudioEngine::runProbe (const float* const* inputs, int numInputs, float* const* chunk, int numOutputs,
                            int offset, int n) noexcept
{
    if (probe.getState() != LatencyProbe::State::Running)
        return;
    const float* in = inputs != nullptr && probeIn >= 0 && probeIn < numInputs && inputs[probeIn] != nullptr
                          ? inputs[probeIn] + offset : nullptr;
    float* out = ! probeMidi && probeOut >= 0 && probeOut < numOutputs ? chunk[probeOut] : nullptr;
    const int at = probe.process (in, out, n);
    if (probeMidi && at >= 0)
    {
        // Leaves when sample `at` reaches the speakers, like any module note with 0 ms offset.
        const double nsPerSample = 1.0e9 / sampleRate;
        const auto due = chunkStartNs + (std::int64_t) (at * nsPerSample) + outputLatencyNs.load (std::memory_order_relaxed);
        const auto ch = (std::uint8_t) (probeChannel - 1);
        pushMidi (due, probePort, MidiBytes::make ((std::uint8_t) (0x90 | ch), (std::uint8_t) probeNote, 110));
        pushMidi (due + 150'000'000, probePort, MidiBytes::make ((std::uint8_t) (0x80 | ch), (std::uint8_t) probeNote, 0));
    }
}

void AudioEngine::publishProbe() noexcept
{
    probeState.store ((int) probe.getState(), std::memory_order_relaxed);
    probeFailure.store ((int) probe.getFailure(), std::memory_order_relaxed);
    probeDelay.store (probe.getDelaySamples(), std::memory_order_relaxed);
    probeSpread.store (probe.getSpreadSamples(), std::memory_order_relaxed);
    probeValid.store (probe.getValidPings(), std::memory_order_relaxed);
    probePings.store (probe.getPingsDone(), std::memory_order_relaxed);
    probeIsMidi.store (probeMidi, std::memory_order_relaxed);
}

ProbeStatus AudioEngine::getProbeStatus() const noexcept
{
    ProbeStatus s;
    s.state = (LatencyProbe::State) probeState.load (std::memory_order_relaxed);
    s.failure = (LatencyProbe::Failure) probeFailure.load (std::memory_order_relaxed);
    s.delaySamples = probeDelay.load (std::memory_order_relaxed);
    s.spreadSamples = probeSpread.load (std::memory_order_relaxed);
    s.validPings = probeValid.load (std::memory_order_relaxed);
    s.pingsDone = probePings.load (std::memory_order_relaxed);
    s.midi = probeIsMidi.load (std::memory_order_relaxed);
    s.runId = probeRun.load (std::memory_order_relaxed);
    return s;
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

AudioEngine::~AudioEngine()
{
    delete pendingPlan.exchange (nullptr);
    delete currentPlan;
    collectGarbage();
}

void AudioEngine::setPlan (std::unique_ptr<PlaybackPlan> plan, double planSampleRate)
{
    pendingPlanRate.store (planSampleRate, std::memory_order_relaxed);
    // If the audio thread has not taken the previous pending plan yet, it never will: free it here.
    delete pendingPlan.exchange (plan.release(), std::memory_order_acq_rel);
}

void AudioEngine::collectGarbage()
{
    PlaybackPlan* p = nullptr;
    while (retiredPlans.pop (p))
        delete p;
}

void AudioEngine::setTrackGain (int slot, float g) noexcept { if (slot >= 0 && slot < maxTracks) trackParams[(size_t) slot].gain.store (std::max (0.0f, g), std::memory_order_relaxed); }
void AudioEngine::setTrackPan (int slot, float p) noexcept  { if (slot >= 0 && slot < maxTracks) trackParams[(size_t) slot].pan.store (std::clamp (p, -1.0f, 1.0f), std::memory_order_relaxed); }
void AudioEngine::setTrackMute (int slot, bool m) noexcept  { if (slot >= 0 && slot < maxTracks) trackParams[(size_t) slot].mute.store (m, std::memory_order_relaxed); }
void AudioEngine::setTrackSolo (int slot, bool so) noexcept { if (slot >= 0 && slot < maxTracks) trackParams[(size_t) slot].solo.store (so, std::memory_order_relaxed); }

float AudioEngine::takeTrackPeak (int slot) noexcept
{
    return (slot >= 0 && slot < maxTracks) ? trackParams[(size_t) slot].peak.exchange (0.0f, std::memory_order_relaxed) : 0.0f;
}

void AudioEngine::setMonitorMix (float click, float guide, float main) noexcept
{
    monitorClick.store (std::max (0.0f, click), std::memory_order_relaxed);
    monitorGuide.store (std::max (0.0f, guide), std::memory_order_relaxed);
    monitorMain.store (std::max (0.0f, main), std::memory_order_relaxed);
}

void AudioEngine::renderTracks (int n) noexcept
{
    const auto& plan = *currentPlan;
    const std::int64_t pos = position.load (std::memory_order_relaxed);

    const bool anySolo = anySoloActive();

    const float invN = 1.0f / (float) n;

    for (const auto& t : plan.tracks)
    {
        if (t.slot < 0 || t.slot >= maxTracks)
            continue;
        auto& p = trackParams[(size_t) t.slot];

        // Click and guide are solo-safe: soloing a backing track never kills the drummer's click.
        const bool silenced = p.mute.load (std::memory_order_relaxed)
                           || (anySolo && t.role == TrackRole::Audio && ! p.solo.load (std::memory_order_relaxed));
        const float gain = silenced ? 0.0f : p.gain.load (std::memory_order_relaxed);
        const float pan = p.pan.load (std::memory_order_relaxed);

        const BusKind bus = t.role == TrackRole::Click ? BusKind::Click
                          : t.role == TrackRole::Guide ? BusKind::Guide
                                                       : t.bus;
        const bool monoBus = OutputRouting::channelsForBus (bus) == 1;
        float* outL = busBuffers[(size_t) bus][0].data();
        float* outR = busBuffers[(size_t) bus][1].data();

        // Target gains. Mono sources use a -3 dB constant-power pan; stereo sources a balance control.
        float& curL = smoothL[(size_t) t.slot];
        float& curR = smoothR[(size_t) t.slot];
        float peak = 0.0f;

        for (const auto& clip : t.clips)
        {
            if (clip.data == nullptr || clip.data->numFrames <= 0)
                continue;
            const auto& d = *clip.data;
            const std::int64_t clipStart = clip.startSample, clipEnd = clip.startSample + d.numFrames;
            const std::int64_t from = std::max (pos, clipStart), to = std::min (pos + n, clipEnd);
            if (from >= to)
                continue;

            const bool monoSrc = d.numChannels == 1;
            const float theta = (pan + 1.0f) * 0.785398163f;
            const float tL = monoBus ? gain : gain * (monoSrc ? std::cos (theta) : (pan <= 0.0f ? 1.0f : 1.0f - pan));
            const float tR = monoBus ? gain : gain * (monoSrc ? std::sin (theta) : (pan >= 0.0f ? 1.0f : 1.0f + pan));
            const float* srcL = d.channel (0);
            const float* srcR = d.channel (1);
            const int o0 = (int) (from - pos), o1 = (int) (to - pos);
            const std::int64_t s0 = from - clipStart;

            for (int i = o0; i < o1; ++i)
            {
                const float k = (float) (i + 1) * invN;   // linear ramp across the block: no zipper noise
                const float gL = curL + (tL - curL) * k;
                const float gR = curR + (tR - curR) * k;
                const auto si = (size_t) (s0 + (i - o0));
                const float l = srcL[si], r = srcR[si];
                if (monoBus)
                {
                    const float m = 0.5f * (l * gL + r * gR);   // mono source: r == l
                    outL[i] += m;
                    peak = std::max (peak, std::abs (m));
                }
                else
                {
                    const float a = l * gL, b = r * gR;
                    outL[i] += a;
                    outR[i] += b;
                    peak = std::max (peak, std::max (std::abs (a), std::abs (b)));
                }
            }
            curL = tL;
            curR = tR;
        }

        atomicMax (p.peak, peak);
    }
}

bool AudioEngine::anySoloActive() const noexcept
{
    const auto& plan = *currentPlan;
    for (const auto& t : plan.tracks)
        if (t.role == TrackRole::Audio && t.slot >= 0 && t.slot < maxTracks && trackParams[(size_t) t.slot].solo.load (std::memory_order_relaxed))
            return true;
    for (const auto& t : plan.midiTracks)
        if (t.slot >= 0 && t.slot < maxTracks && trackParams[(size_t) t.slot].solo.load (std::memory_order_relaxed))
            return true;
    for (const auto& t : plan.instrumentTracks)
        if (t.slot >= 0 && t.slot < maxTracks && trackParams[(size_t) t.slot].solo.load (std::memory_order_relaxed))
            return true;
    return false;
}

void AudioEngine::pushMidi (std::int64_t dueNs, int port, const MidiBytes& m) noexcept
{
    if (port < 0 || port >= maxMidiPorts)
        return;
    MidiOutEvent e;
    e.dueNs = dueNs;
    e.port = (std::uint8_t) port;
    e.msg = m;
    if (midiOut.push (e))
        midiNotes[(size_t) port].observe (m);
    else
        midiDropped.fetch_add (1, std::memory_order_relaxed);   // MIDI thread stalled: never block audio
}

void AudioEngine::releaseMidiNotes() noexcept
{
    const auto now = nowNs();
    for (int port = 0; port < maxMidiPorts; ++port)
    {
        auto& tr = midiNotes[(size_t) port];
        // Copy first: releaseAll() resets the tracker, and pushMidi() would re-observe into it.
        auto copy = tr;
        tr.reset();
        copy.releaseAll ([&] (const MidiBytes& m)
        {
            MidiOutEvent e;
            e.dueNs = now;
            e.port = (std::uint8_t) port;
            e.msg = m;
            if (! midiOut.push (e))
                midiDropped.fetch_add (1, std::memory_order_relaxed);
        });
        activeNotesTelemetry[(size_t) port].store (0, std::memory_order_relaxed);
    }
    stopMidiClock();
    releaseInstrumentNotes();
    // Whatever plays next (resume, new position) starts with a chase of the state there.
    midiExpectedPos = -1;
    instExpectedPos = -1;
}

void AudioEngine::releaseInstrumentNotes() noexcept
{
    if (currentPlan == nullptr || ! currentPlanValid || instL.empty())
        return;
    const auto& plan = *currentPlan;
    const int n = std::min (maxBlock, 64);
    for (size_t ti = 0; ti < plan.instrumentTracks.size() && ti < (size_t) maxTracks; ++ti)
    {
        auto& tr = instNotes[ti];
        const auto& t = plan.instrumentTracks[ti];
        if (! tr.anyActive() || t.instrument == nullptr)
        {
            tr.reset();
            continue;
        }
        int count = 0;
        tr.releaseAll ([&] (const MidiBytes& m)
        {
            if (count < maxInstrumentEvents)
            {
                instMsgs[(size_t) count] = m;
                instOffsets[(size_t) count] = 0;
                ++count;
            }
        });
        // The instrument hears the releases now; this short block is not mixed (we are stopping or jumping).
        t.instrument->render (instL.data(), instR.data(), n, instMsgs.data(), instOffsets.data(), count);
        instActiveTelemetry[ti].store (0, std::memory_order_relaxed);
    }
}

void AudioEngine::renderInstruments (int n) noexcept
{
    const auto& plan = *currentPlan;
    if (plan.instrumentTracks.empty())
        return;

    const std::int64_t pos = position.load (std::memory_order_relaxed);
    const bool reseek = pos != instExpectedPos;
    const bool anySolo = anySoloActive();
    const float invN = 1.0f / (float) n;

    for (size_t ti = 0; ti < plan.instrumentTracks.size() && ti < (size_t) maxTracks; ++ti)
    {
        const auto& t = plan.instrumentTracks[ti];
        if (t.instrument == nullptr || t.slot < 0 || t.slot >= maxTracks)
            continue;
        auto& cursor = instCursor[ti];
        if (reseek)
            cursor = (size_t) (std::lower_bound (t.events.begin(), t.events.end(), pos,
                                                 [] (const MidiSampleEvent& e, std::int64_t p) { return e.sample < p; })
                               - t.events.begin());
        auto& p = trackParams[(size_t) t.slot];
        const bool silenced = p.mute.load (std::memory_order_relaxed)
                              || (anySolo && ! p.solo.load (std::memory_order_relaxed));
        auto& notes = instNotes[ti];

        // PDC: an instrument whose sound comes out L samples late gets its MIDI L samples early.
        // Events between the jump point and pos + L right after a locate are fed at once.
        const auto lat = (std::int64_t) std::clamp (t.instrument->getLatencySamples(), 0, (int) sampleRate);

        int count = 0;
        if (reseek && pos > 0 && fadeAction == FadeAction::None)   // never while stopping
            chaser.chase (t.chase, pos, ! silenced, (std::int64_t) (chaseMinRemainingSec * sampleRate), [&] (const MidiBytes& m)
            {
                if (count >= maxInstrumentEvents)
                    return;
                instMsgs[(size_t) count] = m;
                instOffsets[(size_t) count] = 0;
                notes.observe (m);
                ++count;
            });
        while (cursor < t.events.size() && t.events[cursor].sample < pos + n + lat)
        {
            const auto& e = t.events[cursor++];
            if (silenced && e.msg.isNoteOn())
                continue;   // muted: no new notes; releases and controllers still reach the instrument
            if (e.msg.isNoteOff() && ! notes.isNoteOn (e.msg.channel(), e.msg.data[1]))
                continue;
            if (count >= maxInstrumentEvents)
            {
                instrumentEventsDropped.fetch_add (1, std::memory_order_relaxed);
                continue;
            }
            instMsgs[(size_t) count] = e.msg;
            instOffsets[(size_t) count] = (int) std::clamp<std::int64_t> (e.sample - lat - pos, 0, n - 1);
            notes.observe (e.msg);
            ++count;
        }

        if (! t.instrument->render (instL.data(), instR.data(), n, instMsgs.data(), instOffsets.data(), count))
            continue;   // failed instrument: silent

        const float gain = silenced ? 0.0f : p.gain.load (std::memory_order_relaxed);
        const float pan = p.pan.load (std::memory_order_relaxed);
        const bool monoBus = OutputRouting::channelsForBus (t.bus) == 1;
        const float tL = monoBus ? gain : gain * (pan <= 0.0f ? 1.0f : 1.0f - pan);
        const float tR = monoBus ? gain : gain * (pan >= 0.0f ? 1.0f : 1.0f + pan);
        float& curL = smoothL[(size_t) t.slot];
        float& curR = smoothR[(size_t) t.slot];
        float* outL = busBuffers[(size_t) t.bus][0].data();
        float* outR = busBuffers[(size_t) t.bus][1].data();
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float k = (float) (i + 1) * invN;
            const float a = instL[(size_t) i] * (curL + (tL - curL) * k);
            const float b = instR[(size_t) i] * (curR + (tR - curR) * k);
            if (monoBus)
            {
                const float m = 0.5f * (a + b);
                outL[i] += m;
                peak = std::max (peak, std::abs (m));
            }
            else
            {
                outL[i] += a;
                outR[i] += b;
                peak = std::max (peak, std::max (std::abs (a), std::abs (b)));
            }
        }
        curL = tL;
        curR = tR;
        atomicMax (p.peak, peak);
        instActiveTelemetry[ti].store (notes.activeCount(), std::memory_order_relaxed);
    }
    instExpectedPos = pos + n;
}

void AudioEngine::renderMidi (int n) noexcept
{
    const auto& plan = *currentPlan;
    if (plan.midiTracks.empty())
        return;

    const std::int64_t pos = position.load (std::memory_order_relaxed);
    const bool reseek = pos != midiExpectedPos;
    const bool anySolo = anySoloActive();
    const auto latency = outputLatencyNs.load (std::memory_order_relaxed);
    const double nsPerSample = 1.0e9 / sampleRate;

    for (size_t ti = 0; ti < plan.midiTracks.size() && ti < (size_t) maxTracks; ++ti)
    {
        const auto& t = plan.midiTracks[ti];
        auto& cursor = midiCursor[ti];
        if (reseek)
            cursor = (size_t) (std::lower_bound (t.events.begin(), t.events.end(), pos,
                                                 [] (const MidiSampleEvent& e, std::int64_t p) { return e.sample < p; })
                               - t.events.begin());

        const bool silenced = t.slot >= 0 && t.slot < maxTracks
                              && (trackParams[(size_t) t.slot].mute.load (std::memory_order_relaxed)
                                  || (anySolo && ! trackParams[(size_t) t.slot].solo.load (std::memory_order_relaxed)));
        const auto early = (std::int64_t) (t.sendEarlyMs * 1.0e6);
        // Lookahead: a module slower than the output latency needs its notes before this block is
        // even due, so events up to (early - latency) ahead are queued now (docs/04 §4).
        const auto aheadSamples = early > latency ? (std::int64_t) ((double) (early - latency) / nsPerSample) + 1 : 0;

        if (reseek && pos > 0 && fadeAction == FadeAction::None)   // never while stopping
        {
            // Locate chase: program / controllers as they were at pos, and held long notes again
            // (not on a muted track). Sent at the start of this block.
            const auto due = std::max (chunkStartNs, chunkStartNs + latency - early);
            chaser.chase (t.chase, pos, ! silenced, (std::int64_t) (chaseMinRemainingSec * sampleRate),
                          [&] (const MidiBytes& m) { pushMidi (due, t.port, m); });
        }

        while (cursor < t.events.size() && t.events[cursor].sample < pos + n + aheadSamples)
        {
            const auto& e = t.events[cursor++];
            if (silenced && e.msg.isNoteOn())
                continue;   // muted: no new notes, but note-offs and controllers still go out
            if (e.msg.isNoteOff() && ! midiNotes[(size_t) t.port].isNoteOn (e.msg.channel(), e.msg.data[1]))
                continue;   // its note-on was skipped (muted) or already released
            auto due = chunkStartNs + (std::int64_t) ((double) (e.sample - pos) * nsPerSample) + latency - early;
            pushMidi (std::max (due, chunkStartNs), t.port, e.msg);
        }
    }
    midiExpectedPos = pos + n;

    for (const auto& t : plan.midiTracks)
        if (t.port >= 0 && t.port < maxMidiPorts)
            activeNotesTelemetry[(size_t) t.port].store (midiNotes[(size_t) t.port].activeCount(), std::memory_order_relaxed);
}

void AudioEngine::stopMidiClock() noexcept
{
    // Every stop / pause / locate / song change goes through here: slaves stop, and the next
    // render restarts them with Song Position + Continue at the new position.
    if (clockRunning && currentPlan != nullptr)
    {
        const auto now = nowNs();
        for (const auto& c : currentPlan->midiClocks)
            pushMidi (now, c.port, MidiBytes::make (0xfc));
    }
    clockRunning = false;
    clockNeedsStart = false;
    clockNextTick = -1;
    clockExpectedPos = -1;
}

void AudioEngine::renderMidiClock (int n) noexcept
{
    const auto& plan = *currentPlan;
    if (plan.midiClocks.empty())
        return;

    const std::int64_t pos = position.load (std::memory_order_relaxed);
    if (pos != clockExpectedPos && clockNextTick >= 0)
        stopMidiClock();   // the position jumped without a locate command (loop / song change)
    if (clockNextTick < 0)
    {
        // (Re)start on the next 16th note: Song Position Pointer counts 16ths (6 clocks each),
        // so the slave's first clock after Continue is exactly at that 16th.
        const double beat = plan.tempo.beatAtSeconds ((double) pos / sampleRate);
        const auto firstTick = (std::int64_t) std::ceil (beat * 24.0 - 1e-6);
        clockNextTick = ((firstTick + 5) / 6) * 6;
        clockNeedsStart = true;
    }

    const auto latency = outputLatencyNs.load (std::memory_order_relaxed);
    const double nsPerSample = 1.0e9 / sampleRate;
    for (int guard = 0; guard < 64; ++guard)   // 24 PPQN at 300 BPM is 120 clocks/s: a few per block
    {
        const auto s = (std::int64_t) std::llround (plan.tempo.secondsAtBeat ((double) clockNextTick / 24.0) * sampleRate);
        if (s >= pos + n)
            break;
        const auto base = chunkStartNs + (std::int64_t) ((double) std::max<std::int64_t> (0, s - pos) * nsPerSample) + latency;
        for (const auto& c : plan.midiClocks)
        {
            const auto due = std::max (chunkStartNs, base - (std::int64_t) (c.sendEarlyMs * 1.0e6));
            if (clockNeedsStart)
            {
                if (clockNextTick == 0)
                {
                    pushMidi (due, c.port, MidiBytes::make (0xfa));   // Start
                }
                else
                {
                    const auto spp = std::min<std::int64_t> (clockNextTick / 6, 16383);
                    pushMidi (due, c.port, MidiBytes::make (0xf2, (std::uint8_t) (spp & 0x7f), (std::uint8_t) ((spp >> 7) & 0x7f)));
                    pushMidi (due, c.port, MidiBytes::make (0xfb));   // Continue
                }
            }
            pushMidi (due, c.port, MidiBytes::make (0xf8));
        }
        clockNeedsStart = false;
        clockRunning = true;
        ++clockNextTick;
    }
    clockExpectedPos = pos + n;
}

void AudioEngine::renderClick (int n) noexcept
{
    const auto& plan = *currentPlan;
    if (! plan.hasClick || plan.click.accent.empty() || plan.click.normal.empty())
        return;

    float gain = 1.0f;
    if (plan.clickSlot >= 0 && plan.clickSlot < maxTracks)
    {
        auto& p = trackParams[(size_t) plan.clickSlot];
        gain = p.mute.load (std::memory_order_relaxed) ? 0.0f : p.gain.load (std::memory_order_relaxed);
    }

    float* out = busBuffers[(size_t) BusKind::Click][0].data();
    const std::int64_t pos = position.load (std::memory_order_relaxed);
    float peak = 0.0f;

    auto renderVoice = [&] (int from, int to)
    {
        if (clickVoicePos < 0)
            return;
        const auto& snd = clickVoiceAccent ? plan.click.accent : plan.click.normal;
        for (int i = from; i < to && clickVoicePos < (int) snd.size(); ++i)
        {
            const float v = snd[(size_t) clickVoicePos++] * gain;
            out[i] += v;
            peak = std::max (peak, std::abs (v));
        }
        if (clickVoicePos >= (int) snd.size())
            clickVoicePos = -1;
    };

    // Find beats whose sample position falls inside [pos, pos + n).
    const double t0 = (double) pos / sampleRate;
    double beat = plan.tempo.beatAtSeconds (t0);
    auto bb = plan.tempo.barBeatAtBeat (beat);
    double unit = 4.0 / bb.denominator;
    const double barStart = plan.tempo.beatAtBar (bb.bar);
    double candidate = barStart + std::ceil ((beat - barStart) / unit - 1e-9) * unit;

    int cursor = 0;
    for (int guard = 0; guard < 64; ++guard)
    {
        const auto s = (std::int64_t) std::llround (plan.tempo.secondsAtBeat (candidate) * sampleRate);
        if (s >= pos + n)
            break;
        if (s >= pos)
        {
            const int offset = (int) (s - pos);
            renderVoice (cursor, offset);   // finish the previous click up to the new one
            cursor = offset;
            const auto cb = plan.tempo.barBeatAtBeat (candidate + 1e-9);
            clickVoiceAccent = cb.beatInBar == 0;
            clickVoicePos = 0;
        }
        // Next beat, never crossing a bar line where the time signature may change.
        bb = plan.tempo.barBeatAtBeat (candidate + 1e-9);
        unit = 4.0 / bb.denominator;
        candidate = std::min (candidate + unit, plan.tempo.beatAtBar (bb.bar + 1));
    }
    renderVoice (cursor, n);

    if (plan.clickSlot >= 0 && plan.clickSlot < maxTracks)
        atomicMax (trackParams[(size_t) plan.clickSlot].peak, peak);
}

void AudioEngine::renderChunk (float* const* outputs, int numOutputs, int n) noexcept
{
    for (auto& bus : busBuffers)
        for (auto& ch : bus)
            std::fill (ch.begin(), ch.begin() + n, 0.0f);

    const bool playing = (EngineTransport) transport.load (std::memory_order_relaxed) == EngineTransport::Playing;
    const std::int64_t pos = position.load (std::memory_order_relaxed);

    if (playing && currentPlan != nullptr && currentPlanValid)
    {
        if (lastRenderEnd != pos)
            clickVoicePos = -1;   // after a locate, never continue a click from somewhere else
        renderTracks (n);
        renderClick (n);
        renderMidi (n);
        renderMidiClock (n);
        renderInstruments (n);
        lastRenderEnd = pos + n;
    }

    // Monitor (in-ear) bus = its own tracks + click + guide + optional MR.
    {
        const float mc = monitorClick.load (std::memory_order_relaxed);
        const float mg = monitorGuide.load (std::memory_order_relaxed);
        const float mm = monitorMain.load (std::memory_order_relaxed);
        const float* click = busBuffers[(size_t) BusKind::Click][0].data();
        const float* guide = busBuffers[(size_t) BusKind::Guide][0].data();
        const float* mainL = busBuffers[(size_t) BusKind::Main][0].data();
        const float* mainR = busBuffers[(size_t) BusKind::Main][1].data();
        float* monL = busBuffers[(size_t) BusKind::Monitor][0].data();
        float* monR = busBuffers[(size_t) BusKind::Monitor][1].data();
        for (int i = 0; i < n; ++i)
        {
            const float cue = click[i] * mc + guide[i] * mg;
            monL[i] += cue + mainL[i] * mm;
            monR[i] += cue + mainR[i] * mm;
        }
    }

    // Master volume applies to the Main (PA) bus only.
    {
        const float mg = masterGain.load (std::memory_order_relaxed);
        for (auto& ch : busBuffers[(size_t) BusKind::Main])
            for (int i = 0; i < n; ++i)
                ch[(size_t) i] *= mg;
    }

    // Bus -> physical outputs.
    for (int o = 0; o < numOutputs; ++o)
    {
        if (outputs[o] == nullptr)
            continue;
        const int bus = routes[(size_t) o].bus.load (std::memory_order_relaxed);
        if (bus < 0 || bus >= numBusKinds)
            continue;
        if (routes[(size_t) o].isPA.load (std::memory_order_relaxed) && isInEarOnlyBus ((BusKind) bus))
            continue;   // defence in depth: OutputRouting already refuses this mapping
        const int ch = routes[(size_t) o].channel.load (std::memory_order_relaxed);
        const bool monoBus = OutputRouting::channelsForBus ((BusKind) bus) == 1;
        const float* a = busBuffers[(size_t) bus][0].data();
        const float* b = busBuffers[(size_t) bus][1].data();
        float* out = outputs[o];
        if (monoBus || ch == 0)
            for (int i = 0; i < n; ++i) out[i] += a[i];
        else if (ch == 1)
            for (int i = 0; i < n; ++i) out[i] += b[i];
        else
            for (int i = 0; i < n; ++i) out[i] += 0.5f * (a[i] + b[i]);
    }

    renderTest (outputs, numOutputs, 0, n);
}

void AudioEngine::process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples) noexcept
{
    const auto startNs = timeSource != nullptr ? timeSource() : nowNs();
    callbacks.fetch_add (1, std::memory_order_relaxed);
    // MIDI timing reference: when this block's first sample was really due, without the
    // callback's own scheduling jitter (docs/04 §4).
    std::int64_t blockNs = startNs;
    double blockNsPerSample = 1.0e9 / sampleRate;
    if (clockSmoothing.load (std::memory_order_relaxed) && numSamples > 0)
    {
        blockNs = sampleClock.onCallback (startNs, numSamples);
        blockNsPerSample = sampleClock.getNsPerSample();
        clockRelocks.store (sampleClock.getRelockCount(), std::memory_order_relaxed);
    }
    else
    {
        sampleClock.reset();
    }

    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);

    if (! prepared.load (std::memory_order_acquire) || numSamples <= 0)
        return;

    numOutputs = std::min (numOutputs, maxOutputs);

    // 1. Plan swap, only while not playing (song changes happen at stop). Done before
    //    commands so a PLAY queued together with a new plan starts the new song.
    if ((EngineTransport) transport.load (std::memory_order_relaxed) != EngineTransport::Playing)
    {
        if (auto* next = pendingPlan.exchange (nullptr, std::memory_order_acq_rel))
        {
            if (currentPlan != nullptr && ! retiredPlans.push (currentPlan))
            {
                // Retire queue full (UI not collecting): keep the old plan rather than free on the RT thread.
                pendingPlan.store (next, std::memory_order_release);
            }
            else
            {
                releaseMidiNotes();   // also releases the old plan's instruments
                midiExpectedPos = -1;
                instExpectedPos = -1;
                currentPlan = next;
                currentPlanRate = pendingPlanRate.load (std::memory_order_relaxed);
                currentPlanValid = std::abs (currentPlanRate - sampleRate) < 0.5;
                songLength = currentPlan->lengthSamples;
                clickVoicePos = -1;
            }
        }
    }

    // Deferred PLAY (see playAfterStop): the stop fade has finished and any new plan is in place.
    if (playAfterStop && fadeAction == FadeAction::None
        && (EngineTransport) transport.load (std::memory_order_relaxed) != EngineTransport::Playing)
    {
        playAfterStop = false;
        position.store (std::clamp<std::int64_t> (playAfterStopPosition, 0, songLength > 0 ? songLength : playAfterStopPosition),
                        std::memory_order_relaxed);
        EngineCommand play;
        play.type = EngineCommand::Type::Play;
        handleCommand (play);
    }

    // 2. Commands (bounded per block so a flood cannot starve the audio).
    EngineCommand cmd;
    for (int k = 0; k < 64 && commands.pop (cmd); ++k)
        handleCommand (cmd);

    // 3. Render in chunks no larger than the prepared block size (some drivers vary block sizes).
    float* chunkPtrs[maxOutputs];
    for (int offset = 0; offset < numSamples;)
    {
        const int n = std::min (maxBlock, numSamples - offset);
        for (int ch = 0; ch < numOutputs; ++ch)
            chunkPtrs[ch] = outputs[ch] != nullptr ? outputs[ch] + offset : nullptr;

        chunkStartNs = blockNs + (std::int64_t) ((double) offset * blockNsPerSample);
        renderChunk (chunkPtrs, numOutputs, n);
        runProbe (inputs, numInputs, chunkPtrs, numOutputs, offset, n);

        // Transport advances per chunk so tracks and click stay sample-aligned.
        if ((EngineTransport) transport.load (std::memory_order_relaxed) == EngineTransport::Playing)
        {
            auto pos = position.load (std::memory_order_relaxed) + n;
            if (songLength > 0 && pos >= songLength)
            {
                pos = songLength;
                transport.store ((int) EngineTransport::Stopped, std::memory_order_release);
                releaseMidiNotes();
                pushEvent (EngineEvent::Type::SongEnded);
            }
            position.store (pos, std::memory_order_release);
        }
        offset += n;
    }

    if (probe.getState() != LatencyProbe::State::Idle)
        publishProbe();

    // 4. Fades (stop / emergency) applied to every output.
    if (fadeAction != FadeAction::None)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            fadeGain = std::max (0.0f, fadeGain - fadeStep);
            for (int ch = 0; ch < numOutputs; ++ch)
                if (outputs[ch] != nullptr)
                    outputs[ch][i] *= fadeGain;
        }

        if (fadeGain <= 0.0f)
        {
            const bool wasEmergency = fadeAction == FadeAction::Emergency;
            fadeAction = FadeAction::None;
            fadeGain = 1.0f;
            testSignal.store ((int) TestSignal::None, std::memory_order_release);
            testOutput.store (-1, std::memory_order_release);
            transport.store ((int) EngineTransport::Stopped, std::memory_order_release);
            clickVoicePos = -1;
            releaseMidiNotes();
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
    peakLoadTelemetry.store ((float) xruns.getPeakLoad(), std::memory_order_relaxed);
    lateTelemetry.store (xruns.getLateCount(), std::memory_order_relaxed);
    overloadTelemetry.store (xruns.getOverloadCount(), std::memory_order_relaxed);
    if (flags & XrunDetector::LateCallback)
        pushEvent (EngineEvent::Type::LateCallback, (float) xruns.getLoad());
    if (flags & XrunDetector::DspOverload)
        pushEvent (EngineEvent::Type::DspOverload, (float) xruns.getLoad());
}

ClickSound PlaybackPlan::makeDefaultClick (double sampleRate)
{
    auto burst = [sampleRate] (double freq, float level)
    {
        const int len = (int) (0.03 * sampleRate);   // 30 ms
        std::vector<float> v ((size_t) len);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sampleRate;
            const double env = std::exp (-t * 160.0) * std::min (1.0, i / (0.0005 * sampleRate));   // 0.5 ms attack
            v[(size_t) i] = (float) (std::sin (twoPi * freq * t) * env) * level;
        }
        return v;
    };
    ClickSound c;
    c.accent = burst (1800.0, 0.7f);
    c.normal = burst (1200.0, 0.5f);
    return c;
}

} // namespace b4nd
