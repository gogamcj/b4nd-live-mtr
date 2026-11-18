#include "core/SampleClock.h"

#include <cmath>

namespace b4nd
{

void SampleClock::prepare (double rate, double bandwidthHz) noexcept
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    nominalNsPerSample = 1.0e9 / sampleRate;
    nsPerSample = nominalNsPerSample;
    bandwidth = bandwidthHz > 0.0 ? bandwidthHz : 1.0;
    locked = false;
}

std::int64_t SampleClock::onCallback (std::int64_t measuredNs, int numSamples) noexcept
{
    if (numSamples <= 0)
        return measuredNs;
    const double blockNs = numSamples * nominalNsPerSample;
    const double measured = (double) measuredNs;

    if (locked)
    {
        // A callback more than two blocks away from the prediction is not jitter: a dropout or a
        // driver restart. Start again from the measurement instead of slowly drifting there.
        const double err = measured - predictedNs;
        if (std::abs (err) > 2.0 * blockNs + 2.0e6)
        {
            locked = false;
            ++relocks;
        }
    }
    if (! locked)
    {
        locked = true;
        nsPerSample = nominalNsPerSample;
        predictedNs = measured + numSamples * nsPerSample;
        return measuredNs;
    }

    // Loop coefficients for this block length (critically damped second-order loop).
    const double omega = 2.0 * 3.141592653589793 * bandwidth * (numSamples / sampleRate);
    const double b = std::sqrt (2.0) * omega;
    const double c = omega * omega;

    const double err = measured - predictedNs;
    const double blockStart = predictedNs + b * err;
    nsPerSample += c * err / numSamples;
    // The interface clock is within a fraction of a percent of nominal: never let a burst of bad
    // measurements drag the rate estimate further than that.
    const double lo = nominalNsPerSample * 0.99, hi = nominalNsPerSample * 1.01;
    nsPerSample = nsPerSample < lo ? lo : (nsPerSample > hi ? hi : nsPerSample);
    predictedNs = blockStart + numSamples * nsPerSample;
    return (std::int64_t) std::llround (blockStart);
}

} // namespace b4nd
