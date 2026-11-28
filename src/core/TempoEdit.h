#pragma once

#include "core/TempoMap.h"

#include <cstddef>

namespace b4nd
{

/**
    Hand corrections of a song's tempo map (after AI analysis or for a song entered by hand).
    Pure functions on a copy: the caller swaps the result in (never on the audio thread).
    Every function returns false and leaves `out` unspecified on invalid input.
*/
namespace tempoEdit
{
    /** Half / double tempo: every BPM times k and every tempo change at beat * k, so that the
        changes stay at the same time in seconds. Time signature changes move to bar * k
        (rounded). k must be 0.5 or 2 (or any value in [0.25, 4]). */
    bool scale (const TempoMap& in, double k, TempoMap& out);

    /** One constant tempo (removes the tempo changes). bpm 20..400. Bar 1 stays where it is. */
    bool constantTempo (const TempoMap& in, double bpm, TempoMap& out);

    /** One time signature for the whole song (removes the changes). Bar 1 stays where it is.
        numerator 1..16, denominator 2, 4, 8 or 16. */
    bool timeSignature (const TempoMap& in, int numerator, int denominator, TempoMap& out);

    /** Moves bar 1 beat 1 by `beats` quarter notes (negative: earlier), at the first tempo.
        If it would land before 0 s, it moves one bar later instead (the same bar lines). */
    bool shiftBarOne (const TempoMap& in, double beats, TempoMap& out);

    /** Bar 1 beat 1 at this time (>= 0 s). */
    bool barOneAt (const TempoMap& in, double seconds, TempoMap& out);
}

/**
    Tap tempo: the median interval of the last taps. A pause of more than 2 s starts over.
    Times in seconds from any monotonic clock. No allocation.
*/
class TapTempo
{
public:
    /** Records a tap; returns the BPM once there are at least 3 taps, else 0. */
    double tap (double nowSeconds) noexcept;
    double bpm() const noexcept;
    int taps() const noexcept { return count; }
    void reset() noexcept { count = 0; }

    static constexpr int maxTaps = 8;
    static constexpr double resetAfterSeconds = 2.0;

private:
    double times[maxTaps] {};
    int count = 0;
};

} // namespace b4nd
