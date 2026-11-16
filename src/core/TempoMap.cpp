#include "core/TempoMap.h"

#include <algorithm>
#include <cmath>

namespace b4nd
{

TempoMap::TempoMap()
{
    tempo    = { TempoPoint { 0.0, 120.0 } };
    timeSigs = { TimeSigPoint { 0, 4, 4 } };
    rebuildCaches();
}

bool TempoMap::setTempoPoints (std::vector<TempoPoint> points)
{
    if (points.empty())
        return false;

    for (const auto& p : points)
        if (! std::isfinite (p.bpm) || p.bpm <= 0.0 || ! std::isfinite (p.beat) || p.beat < 0.0)
            return false;

    std::stable_sort (points.begin(), points.end(),
                      [] (const TempoPoint& a, const TempoPoint& b) { return a.beat < b.beat; });

    // Drop duplicates at the same beat (last one wins).
    std::vector<TempoPoint> unique;
    for (const auto& p : points)
    {
        if (! unique.empty() && unique.back().beat == p.beat)
            unique.back() = p;
        else
            unique.push_back (p);
    }

    if (unique.front().beat > 0.0)
        unique.insert (unique.begin(), TempoPoint { 0.0, unique.front().bpm });

    tempo = std::move (unique);
    rebuildCaches();
    return true;
}

bool TempoMap::setTimeSigPoints (std::vector<TimeSigPoint> points)
{
    if (points.empty())
        return false;

    for (const auto& p : points)
    {
        const bool denOk = p.denominator >= 1 && p.denominator <= 64
                        && (p.denominator & (p.denominator - 1)) == 0;
        if (p.numerator < 1 || p.numerator > 64 || ! denOk || p.bar < 0)
            return false;
    }

    std::stable_sort (points.begin(), points.end(),
                      [] (const TimeSigPoint& a, const TimeSigPoint& b) { return a.bar < b.bar; });

    std::vector<TimeSigPoint> unique;
    for (const auto& p : points)
    {
        if (! unique.empty() && unique.back().bar == p.bar)
            unique.back() = p;
        else
            unique.push_back (p);
    }

    if (unique.front().bar > 0)
    {
        auto first = unique.front();
        first.bar = 0;
        unique.insert (unique.begin(), first);
    }

    timeSigs = std::move (unique);
    rebuildCaches();
    return true;
}

void TempoMap::rebuildCaches()
{
    tempoStartSeconds.assign (tempo.size(), 0.0);
    for (std::size_t i = 1; i < tempo.size(); ++i)
    {
        const double beats = tempo[i].beat - tempo[i - 1].beat;
        tempoStartSeconds[i] = tempoStartSeconds[i - 1] + beats * 60.0 / tempo[i - 1].bpm;
    }

    sigStartBeat.assign (timeSigs.size(), 0.0);
    for (std::size_t i = 1; i < timeSigs.size(); ++i)
    {
        const auto& prev = timeSigs[i - 1];
        const int bars = timeSigs[i].bar - prev.bar;
        sigStartBeat[i] = sigStartBeat[i - 1] + bars * quarterNotesPerBar (prev.numerator, prev.denominator);
    }
}

std::size_t TempoMap::tempoIndexForBeat (double beat) const noexcept
{
    auto it = std::upper_bound (tempo.begin(), tempo.end(), beat,
                                [] (double b, const TempoPoint& p) { return b < p.beat; });
    return it == tempo.begin() ? 0 : static_cast<std::size_t> (std::distance (tempo.begin(), it) - 1);
}

std::size_t TempoMap::tempoIndexForSeconds (double seconds) const noexcept
{
    auto it = std::upper_bound (tempoStartSeconds.begin(), tempoStartSeconds.end(), seconds);
    return it == tempoStartSeconds.begin() ? 0
                                           : static_cast<std::size_t> (std::distance (tempoStartSeconds.begin(), it) - 1);
}

std::size_t TempoMap::sigIndexForBar (int bar) const noexcept
{
    auto it = std::upper_bound (timeSigs.begin(), timeSigs.end(), bar,
                                [] (int b, const TimeSigPoint& p) { return b < p.bar; });
    return it == timeSigs.begin() ? 0 : static_cast<std::size_t> (std::distance (timeSigs.begin(), it) - 1);
}

double TempoMap::secondsAtBeat (double beat) const noexcept
{
    const auto i = tempoIndexForBeat (beat);
    return tempoStartSeconds[i] + (beat - tempo[i].beat) * 60.0 / tempo[i].bpm;
}

double TempoMap::beatAtSeconds (double seconds) const noexcept
{
    const auto i = tempoIndexForSeconds (seconds);
    return tempo[i].beat + (seconds - tempoStartSeconds[i]) * tempo[i].bpm / 60.0;
}

double TempoMap::bpmAtBeat (double beat) const noexcept
{
    return tempo[tempoIndexForBeat (beat)].bpm;
}

double TempoMap::beatAtBar (int bar) const noexcept
{
    const auto i = sigIndexForBar (bar);
    const auto& s = timeSigs[i];
    return sigStartBeat[i] + (bar - s.bar) * quarterNotesPerBar (s.numerator, s.denominator);
}

BarBeat TempoMap::barBeatAtBeat (double beat) const noexcept
{
    // Find the time-signature segment containing this beat.
    auto it = std::upper_bound (sigStartBeat.begin(), sigStartBeat.end(), beat);
    const std::size_t i = it == sigStartBeat.begin() ? 0
                                                     : static_cast<std::size_t> (std::distance (sigStartBeat.begin(), it) - 1);
    const auto& s = timeSigs[i];
    const double qnPerBar  = quarterNotesPerBar (s.numerator, s.denominator);
    const double qnPerBeat = 4.0 / s.denominator;

    const double rel = beat - sigStartBeat[i];
    // Small epsilon guards against 3.9999999 → bar 0 due to floating point error.
    const double eps = 1e-9;
    const int barsIn = static_cast<int> (std::floor ((rel + eps) / qnPerBar));
    const double inBar = std::max (0.0, rel - barsIn * qnPerBar);
    const double beatPos = (inBar + eps) / qnPerBeat;
    const int beatIdx = std::min (s.numerator - 1, static_cast<int> (std::floor (beatPos)));

    BarBeat r;
    r.bar = s.bar + barsIn;
    r.beatInBar = beatIdx;
    r.fraction = std::clamp (inBar / qnPerBeat - beatIdx, 0.0, 1.0);
    r.numerator = s.numerator;
    r.denominator = s.denominator;
    return r;
}

} // namespace b4nd
