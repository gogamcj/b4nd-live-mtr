#include "Limiter.h"

#include <algorithm>
#include <cmath>

namespace b4nd
{

void Limiter::prepare (double sampleRate, double lookaheadMs, double releaseMs) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    lookahead = std::clamp ((int) std::lround (lookaheadMs * sr / 1000.0), 1, maxLookahead - 1);
    releaseCoef = 1.0 - std::exp (-1.0 / (std::max (1.0, releaseMs) * sr / 1000.0));
    reset();
}

void Limiter::reset() noexcept
{
    delayL.fill (0.0f);
    delayR.fill (0.0f);
    delayPos = 0;
    qHead = qTail = 0;
    index = 0;
    released = 1.0f;
    box.fill (1.0f);
    boxSum = (double) lookahead;
    boxPos = 0;
    minGain = 1.0f;
}

void Limiter::setCeilingDb (float db) noexcept
{
    ceiling = std::pow (10.0f, std::clamp (db, -20.0f, 0.0f) / 20.0f);
}

void Limiter::process (float* left, float* right, int n) noexcept
{
    const bool mono = left == right;
    for (int i = 0; i < n; ++i)
    {
        float l = left[i], r = mono ? l : right[i];
        if (! std::isfinite (l)) l = 0.0f;   // a broken plugin never reaches the PA as noise
        if (! std::isfinite (r)) r = 0.0f;

        // 1. Gain this sample needs.
        const float peak = std::max (std::abs (l), std::abs (r));
        const float need = peak > ceiling ? ceiling / peak : 1.0f;

        // 2. Minimum over the last `lookahead` samples (monotonic queue).
        while (qTail != qHead && qVal[(size_t) ((qTail - 1 + maxLookahead) % maxLookahead)] >= need)
            qTail = (qTail - 1 + maxLookahead) % maxLookahead;
        qVal[(size_t) qTail] = need;
        qIdx[(size_t) qTail] = index;
        qTail = (qTail + 1) % maxLookahead;
        while (qIdx[(size_t) qHead] < index - lookahead)   // window: this sample and `lookahead` before it
            qHead = (qHead + 1) % maxLookahead;
        const float held = qVal[(size_t) qHead];
        ++index;

        // 3. Instant attack (the window already looks ahead), slow release.
        released = held < released ? held : released + (float) ((held - released) * releaseCoef);

        // 4. Average over the window: the gain ramps down smoothly and reaches `need` exactly
        //    when the peak leaves the delay line.
        boxSum += (double) released - (double) box[(size_t) boxPos];
        box[(size_t) boxPos] = released;
        boxPos = (boxPos + 1) % lookahead;
        const float g = std::min (1.0f, (float) (boxSum / (double) lookahead));
        minGain = std::min (minGain, g);

        // 5. Delay line out, gain, final safety clip.
        const float dl = delayL[(size_t) delayPos], dr = delayR[(size_t) delayPos];
        delayL[(size_t) delayPos] = l;
        delayR[(size_t) delayPos] = r;
        delayPos = (delayPos + 1) % lookahead;
        const float ol = dl * g, orr = dr * g;
        if (std::abs (ol) > ceiling * 1.0001f || std::abs (orr) > ceiling * 1.0001f)
            ++safetyClips;
        left[i] = std::clamp (ol, -ceiling, ceiling);
        if (! mono)
            right[i] = std::clamp (orr, -ceiling, ceiling);
    }
    if (index > ((std::int64_t) 1 << 60))   // never in practice; keeps the queue indices sane
        reset();
}

} // namespace b4nd
