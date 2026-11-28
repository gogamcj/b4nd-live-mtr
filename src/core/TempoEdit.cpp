#include "core/TempoEdit.h"

#include <algorithm>
#include <cmath>

namespace b4nd
{
namespace tempoEdit
{
namespace
{
bool build (std::vector<TempoPoint> tempo, std::vector<TimeSigPoint> sigs, double offset, TempoMap& out)
{
    TempoMap m;
    if (! m.setTempoPoints (std::move (tempo)) || ! m.setTimeSigPoints (std::move (sigs)) || ! m.setOffsetSeconds (offset))
        return false;
    out = m;
    return true;
}
} // namespace

bool scale (const TempoMap& in, double k, TempoMap& out)
{
    if (! std::isfinite (k) || k < 0.25 || k > 4.0)
        return false;
    std::vector<TempoPoint> pts;
    for (const auto& p : in.getTempoPoints())
    {
        const double bpm = p.bpm * k;
        if (bpm < 20.0 || bpm > 400.0)
            return false;
        pts.push_back ({ p.beat * k, bpm });
    }
    std::vector<TimeSigPoint> sigs;
    for (const auto& s : in.getTimeSigPoints())
    {
        const int bar = (int) std::lround (s.bar * k);
        if (! sigs.empty() && bar <= sigs.back().bar)
            continue;   // halving squeezed two changes into one bar: keep the first
        sigs.push_back ({ bar, s.numerator, s.denominator });
    }
    return build (std::move (pts), std::move (sigs), in.getOffsetSeconds(), out);
}

bool constantTempo (const TempoMap& in, double bpm, TempoMap& out)
{
    if (! std::isfinite (bpm) || bpm < 20.0 || bpm > 400.0)
        return false;
    return build ({ { 0.0, bpm } }, in.getTimeSigPoints(), in.getOffsetSeconds(), out);
}

bool timeSignature (const TempoMap& in, int numerator, int denominator, TempoMap& out)
{
    if (numerator < 1 || numerator > 16 || (denominator != 2 && denominator != 4 && denominator != 8 && denominator != 16))
        return false;
    return build (in.getTempoPoints(), { { 0, numerator, denominator } }, in.getOffsetSeconds(), out);
}

bool shiftBarOne (const TempoMap& in, double beats, TempoMap& out)
{
    if (! std::isfinite (beats))
        return false;
    const double spb = 60.0 / in.bpmAtBeat (0.0);
    double at = in.getOffsetSeconds() + beats * spb;
    if (at < -1e-9)
    {
        const auto bb = in.barBeatAtBeat (0.0);
        const double bar = TempoMap::quarterNotesPerBar (bb.numerator, bb.denominator) * spb;
        while (at < -1e-9)
            at += bar;
    }
    return barOneAt (in, std::max (0.0, at), out);
}

bool barOneAt (const TempoMap& in, double seconds, TempoMap& out)
{
    if (! std::isfinite (seconds) || seconds < 0.0)
        return false;
    return build (in.getTempoPoints(), in.getTimeSigPoints(), seconds, out);
}
} // namespace tempoEdit

double TapTempo::tap (double now) noexcept
{
    if (count > 0 && (now - times[count - 1] > resetAfterSeconds || now <= times[count - 1]))
        count = 0;
    if (count == maxTaps)
    {
        for (int i = 1; i < maxTaps; ++i)
            times[i - 1] = times[i];
        --count;
    }
    times[count++] = now;
    return bpm();
}

double TapTempo::bpm() const noexcept
{
    if (count < 3)
        return 0.0;
    double d[maxTaps - 1];
    const int n = count - 1;
    for (int i = 0; i < n; ++i)
        d[i] = times[i + 1] - times[i];
    std::sort (d, d + n);
    const double med = n % 2 == 1 ? d[n / 2] : 0.5 * (d[n / 2 - 1] + d[n / 2]);
    return med > 0.0 ? 60.0 / med : 0.0;
}

} // namespace b4nd
