#pragma once

#include "core/Midi.h"

#include <array>
#include <cstdint>
#include <vector>

namespace b4nd
{

/**
    Locate chase (docs/04 §6): after a jump into the middle of a song, a module or instrument must
    sound as if it had played from the start: the last bank / program / controllers / pitch bend
    before the jump point are sent again, and long notes that are still held there (pads, strings)
    start again.

    The index is built once per track on the message thread (when the plan is built); chase() runs
    on the audio thread with a fixed scratch area and bounded work: no allocation.
*/
struct ChaseIndex
{
    struct Span
    {
        std::int64_t start = 0, end = 0;   // samples; end = note-off (or the last event)
        MidiBytes on;
    };
    std::vector<MidiSampleEvent> controls;   // CC (not 120-127), program, pitch bend, channel pressure
    std::vector<Span> spans;                 // notes, sorted by start
    std::int64_t longestSpan = 0;

    static ChaseIndex build (const std::vector<MidiSampleEvent>& sortedEvents);
};

class MidiChaser
{
public:
    static constexpr int maxScan = 8192;      // events looked at per chase (bounded work)
    static constexpr int maxMessages = 512;   // messages a chase may produce

    /**
        Messages that recreate the state at `pos` (events at exactly `pos` are not included: they
        play normally). Controls come first in their original order, then the held notes that
        still have at least `minRemaining` samples to go (when `withNotes`).
        `send (const MidiBytes&)` is called for each. Realtime safe.
    */
    template <typename Send>
    void chase (const ChaseIndex& idx, std::int64_t pos, bool withNotes, std::int64_t minRemaining, Send&& send) noexcept
    {
        seen.fill (0);
        int count = 0;

        // Controls: walk back from pos, keep the newest value of each (channel, controller).
        std::size_t lo = 0, hi = idx.controls.size();
        while (lo < hi)
        {
            const auto mid = (lo + hi) / 2;
            if (idx.controls[mid].sample < pos) lo = mid + 1; else hi = mid;
        }
        int scanned = 0;
        for (std::size_t i = lo; i-- > 0 && scanned < maxScan && count < maxMessages; ++scanned)
        {
            const auto& m = idx.controls[i].msg;
            const int key = slotOf (m);
            if (key < 0 || isSeen (key))
                continue;
            markSeen (key);
            buffer[(std::size_t) count++] = m;
        }
        for (int i = count; i-- > 0;)   // oldest first: bank select before program change
            send (buffer[(std::size_t) i]);

        if (! withNotes)
            return;
        // Notes held across pos: spans that start before pos within the longest note length.
        lo = 0; hi = idx.spans.size();
        while (lo < hi)
        {
            const auto mid = (lo + hi) / 2;
            if (idx.spans[mid].start < pos) lo = mid + 1; else hi = mid;
        }
        scanned = 0;
        for (std::size_t i = lo; i-- > 0 && scanned < maxScan; ++scanned)
        {
            const auto& s = idx.spans[i];
            if (s.start < pos - idx.longestSpan)
                break;
            if (s.end > pos && s.end - pos >= minRemaining)
                send (s.on);
        }
    }

private:
    static constexpr int slotsPerChannel = 131;   // 128 CCs, program, pitch bend, channel pressure
    static int slotOf (const MidiBytes& m) noexcept
    {
        const int base = m.channel() * slotsPerChannel;
        switch (m.status())
        {
            case 0xb0: return m.data[1] < 120 ? base + m.data[1] : -1;
            case 0xc0: return base + 128;
            case 0xe0: return base + 129;
            case 0xd0: return base + 130;
            default:   return -1;
        }
    }
    bool isSeen (int k) const noexcept { return (seen[(std::size_t) (k >> 6)] >> (k & 63)) & 1u; }
    void markSeen (int k) noexcept     { seen[(std::size_t) (k >> 6)] |= (std::uint64_t) 1 << (k & 63); }

    std::array<std::uint64_t, (16 * slotsPerChannel + 63) / 64> seen {};
    std::array<MidiBytes, maxMessages> buffer {};
};

} // namespace b4nd
