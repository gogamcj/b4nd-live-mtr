#include "core/TimeFormat.h"

#include <cmath>
#include <cstdio>

namespace b4nd
{

std::string formatSeconds (double seconds, bool withMillis)
{
    if (! std::isfinite (seconds))
        return "--:--";

    const bool negative = seconds < 0.0;
    long long totalMs = std::llround (std::fabs (seconds) * 1000.0);
    if (! withMillis)
        totalMs = (totalMs / 1000) * 1000;   // truncate toward zero for clock display

    const long long ms = totalMs % 1000;
    const long long totalSec = totalMs / 1000;
    const long long s = totalSec % 60;
    const long long m = (totalSec / 60) % 60;
    const long long h = totalSec / 3600;

    char buf[48];
    if (h > 0)
    {
        if (withMillis) std::snprintf (buf, sizeof (buf), "%s%lld:%02lld:%02lld.%03lld", negative ? "-" : "", h, m, s, ms);
        else            std::snprintf (buf, sizeof (buf), "%s%lld:%02lld:%02lld", negative ? "-" : "", h, m, s);
    }
    else
    {
        if (withMillis) std::snprintf (buf, sizeof (buf), "%s%lld:%02lld.%03lld", negative ? "-" : "", m, s, ms);
        else            std::snprintf (buf, sizeof (buf), "%s%lld:%02lld", negative ? "-" : "", m, s);
    }
    return buf;
}

std::string formatBarBeat (const BarBeat& bb)
{
    int ticks = static_cast<int> (std::floor (bb.fraction * 960.0));
    if (ticks > 959) ticks = 959;
    if (ticks < 0) ticks = 0;
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%d.%d.%03d", bb.bar + 1, bb.beatInBar + 1, ticks);
    return buf;
}

double totalSeconds (const double* lengths, int count) noexcept
{
    double t = 0.0;
    for (int i = 0; i < count; ++i)
        if (std::isfinite (lengths[i]) && lengths[i] > 0.0)
            t += lengths[i];
    return t;
}

} // namespace b4nd
