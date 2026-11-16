#include "core/XrunDetector.h"

namespace b4nd
{

void XrunDetector::prepare (double sr, double late, double overload, std::int64_t tolNs) noexcept
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    lateFactor = late;
    overloadFactor = overload;
    toleranceNs = tolNs;
    reset();
}

void XrunDetector::reset() noexcept
{
    lastStartNs = -1;
    lastExpectedNs = 0;
    lateCount = overloadCount = 0;
    load = peakLoad = 0.0;
}

std::uint32_t XrunDetector::onCallback (std::int64_t startNs, std::int64_t endNs, int numSamples) noexcept
{
    std::uint32_t result = None;
    if (numSamples <= 0)
        return result;

    const auto blockNs = static_cast<std::int64_t> (numSamples * 1.0e9 / sampleRate);

    if (lastStartNs >= 0)
    {
        // Compare against the previous block's length: that is how long the driver
        // had to wait before it needed us again.
        const auto gap = startNs - lastStartNs;
        if ((double) gap > lateFactor * (double) lastExpectedNs + (double) toleranceNs)
        {
            ++lateCount;
            result |= LateCallback;
        }
    }

    const double thisLoad = blockNs > 0 ? (double) (endNs - startNs) / (double) blockNs : 0.0;
    if (thisLoad > overloadFactor)
    {
        ++overloadCount;
        result |= DspOverload;
    }

    load = load * 0.9 + thisLoad * 0.1;
    if (thisLoad > peakLoad)
        peakLoad = thisLoad;

    lastStartNs = startNs;
    lastExpectedNs = blockNs;
    return result;
}

} // namespace b4nd
