#pragma once

#include <string>

#include "core/TempoMap.h"

namespace b4nd
{

/** "m:ss.mmm" (or "h:mm:ss.mmm" over an hour). Negative values get a leading '-'. */
std::string formatSeconds (double seconds, bool withMillis = true);

/** "bar.beat.tick" 1-based display, ticks 0..959 (PPQ 960 per beat). */
std::string formatBarBeat (const BarBeat& bb);

/** Total of a list of song lengths, for setlist duration display. */
double totalSeconds (const double* lengths, int count) noexcept;

} // namespace b4nd
