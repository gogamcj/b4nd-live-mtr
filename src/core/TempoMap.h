#pragma once

#include <cstddef>
#include <vector>

namespace b4nd
{

/** One tempo change. Tempo is constant ("step") from this beat until the next point.
    Beats are always quarter notes, independent of the time signature. */
struct TempoPoint
{
    double beat = 0.0;
    double bpm  = 120.0;
};

/** Time signature change taking effect at the start of a bar (0-based). */
struct TimeSigPoint
{
    int bar         = 0;
    int numerator   = 4;
    int denominator = 4;
};

struct BarBeat
{
    int    bar = 0;            // 0-based bar index
    int    beatInBar = 0;      // 0-based beat (in time-signature denominator units)
    double fraction = 0.0;     // 0..1 within that beat
    int    numerator = 4;
    int    denominator = 4;
};

/**
    Converts between seconds, quarter-note beats and bars for a song.

    Built on a non-realtime thread (setTempoPoints / setTimeSigPoints allocate);
    the query functions do not allocate and are safe to call from the audio thread
    on an immutable instance.
*/
class TempoMap
{
public:
    TempoMap();

    /** Replaces the tempo points. Points are sorted; a point at beat 0 is enforced
        (the first point's tempo is used if none exists at 0). Invalid BPM values
        (<= 0 or non-finite) are rejected and the function returns false. */
    bool setTempoPoints (std::vector<TempoPoint> points);

    /** Replaces time signature points. A point at bar 0 is enforced. Returns false on
        invalid values (numerator < 1, denominator not a power of two in 1..64). */
    bool setTimeSigPoints (std::vector<TimeSigPoint> points);

    double secondsAtBeat (double beat) const noexcept;
    double beatAtSeconds (double seconds) const noexcept;
    double bpmAtBeat (double beat) const noexcept;

    /** Quarter-note beat where the given bar starts. */
    double beatAtBar (int bar) const noexcept;
    BarBeat barBeatAtBeat (double beat) const noexcept;

    const std::vector<TempoPoint>&   getTempoPoints() const noexcept   { return tempo; }
    const std::vector<TimeSigPoint>& getTimeSigPoints() const noexcept { return timeSigs; }

    /** Length of a bar in quarter notes for a signature. */
    static double quarterNotesPerBar (int numerator, int denominator) noexcept
    {
        return numerator * 4.0 / denominator;
    }

private:
    std::size_t tempoIndexForBeat (double beat) const noexcept;
    std::size_t tempoIndexForSeconds (double seconds) const noexcept;
    std::size_t sigIndexForBar (int bar) const noexcept;
    void rebuildCaches();

    std::vector<TempoPoint>   tempo;
    std::vector<double>       tempoStartSeconds;   // seconds at each tempo point
    std::vector<TimeSigPoint> timeSigs;
    std::vector<double>       sigStartBeat;        // beat at each time sig point
};

} // namespace b4nd
