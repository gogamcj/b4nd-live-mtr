#include "LatencyProbe.h"

#include <algorithm>
#include <cmath>

namespace b4nd
{

namespace
{
    constexpr double noiseSec = 0.2;        // noise floor measured once at the start
    constexpr double quietSec = 0.15;       // input must be quiet this long before each ping
    constexpr double settleMaxSec = 3.0;    // a sound that never decays fails the run
    constexpr float minThreshold = 0.003f;  // about -50 dBFS
    constexpr float maxNoise = 0.1f;        // louder "silence" than -20 dBFS: wrong input or feedback
}

void LatencyProbe::start (const Settings& settings) noexcept
{
    s = settings;
    s.pings = std::clamp (s.pings, 1, maxPings);
    s.level = std::clamp (s.level, 0.0f, 0.5f);
    state = s.sampleRate > 0.0 ? State::Running : State::Failed;
    failure = state == State::Running ? Failure::None : Failure::BadSetup;
    phase = Phase::Noise;
    clock = phaseStart = quietSince = 0;
    stimulusAt = -1;
    noisePeak = threshold = 0.0f;
    // About a 2 kHz square cycle: sharp edges that converters pass well.
    burstLen = std::max (2, (int) (s.sampleRate / 2000.0));
    burstPos = -1;
    pingIndex = valid = spread = 0;
    median = -1;
}

void LatencyProbe::cancel() noexcept
{
    if (state != State::Running)
        return;
    state = State::Failed;
    failure = Failure::Cancelled;
    burstPos = -1;
}

int LatencyProbe::process (const float* in, float* out, int n) noexcept
{
    int stimulusOffset = -1;
    if (state != State::Running)
        return -1;
    if (in == nullptr)
    {
        state = State::Failed;
        failure = Failure::NoSignal;
        return -1;
    }

    const auto sr = s.sampleRate;
    for (int i = 0; i < n && state == State::Running; ++i, ++clock)
    {
        const float x = std::abs (in[i]);

        switch (phase)
        {
            case Phase::Noise:
                noisePeak = std::max (noisePeak, x);
                if ((double) (clock - phaseStart) >= noiseSec * sr)
                {
                    if (noisePeak > maxNoise)
                    {
                        state = State::Failed;
                        failure = Failure::Noisy;
                        break;
                    }
                    threshold = std::max (minThreshold, noisePeak * 4.0f);
                    phase = Phase::Settle;
                    phaseStart = quietSince = clock;
                }
                break;

            case Phase::Settle:
                if (x > threshold)
                    quietSince = clock + 1;
                if ((double) (clock + 1 - quietSince) >= quietSec * sr)
                {
                    // Stimulus at this sample.
                    phase = Phase::Listen;
                    stimulusAt = phaseStart = clock;
                    stimulusOffset = i;
                    if (s.writeBurst)
                        burstPos = 0;
                }
                else if ((double) (clock - phaseStart) >= settleMaxSec * sr)
                {
                    state = State::Failed;
                    failure = Failure::Noisy;
                }
                break;

            case Phase::Listen:
                if (clock > stimulusAt && x > threshold)
                {
                    if (valid < maxPings)
                        delays[(size_t) valid++] = (int) (clock - stimulusAt);
                    ++pingIndex;
                    phase = Phase::Settle;
                    phaseStart = quietSince = clock + 1;
                    if (pingIndex >= s.pings)
                        finish();
                }
                else if ((double) (clock - stimulusAt) >= s.listenSec * sr)
                {
                    ++pingIndex;   // no onset for this ping
                    phase = Phase::Settle;
                    phaseStart = quietSince = clock + 1;
                    if (pingIndex >= s.pings)
                        finish();
                }
                break;
        }

        // The burst plays out across chunk boundaries.
        if (burstPos >= 0)
        {
            if (out != nullptr && state == State::Running)
                out[i] += burstPos < burstLen / 2 ? s.level : -s.level;
            if (++burstPos >= burstLen)
                burstPos = -1;
        }
    }
    return stimulusOffset;
}

void LatencyProbe::finish() noexcept
{
    burstPos = -1;
    if (valid == 0 || valid * 2 <= s.pings)   // most pings must be heard
    {
        state = State::Failed;
        failure = Failure::NoSignal;
        return;
    }
    std::array<int, maxPings> sorted = delays;
    std::sort (sorted.begin(), sorted.begin() + valid);
    median = sorted[(size_t) (valid / 2)];
    spread = sorted[(size_t) (valid - 1)] - sorted[0];
    if ((double) spread > s.maxSpreadSec * s.sampleRate + 1.0)
    {
        state = State::Failed;
        failure = Failure::Unstable;
        return;
    }
    state = State::Done;
}

} // namespace b4nd
