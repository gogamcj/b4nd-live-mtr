#include "core/MidiChase.h"

#include <algorithm>
#include <limits>

namespace b4nd
{

ChaseIndex ChaseIndex::build (const std::vector<MidiSampleEvent>& events)
{
    ChaseIndex idx;
    // Open notes per (channel, note): index into spans (-1 = none).
    std::array<std::array<int, 128>, 16> open;
    for (auto& ch : open) ch.fill (-1);

    for (const auto& e : events)
    {
        const auto st = e.msg.status();
        const int ch = e.msg.channel();
        if (e.msg.isNoteOn())
        {
            auto& o = open[(std::size_t) ch][e.msg.data[1]];
            if (o >= 0)
                idx.spans[(std::size_t) o].end = e.sample;   // re-struck: the previous one ends here
            o = (int) idx.spans.size();
            idx.spans.push_back ({ e.sample, std::numeric_limits<std::int64_t>::max(), e.msg });
        }
        else if (e.msg.isNoteOff())
        {
            auto& o = open[(std::size_t) ch][e.msg.data[1]];
            if (o >= 0)
                idx.spans[(std::size_t) o].end = e.sample;
            o = -1;
        }
        else if ((st == 0xb0 && e.msg.data[1] < 120) || st == 0xc0 || st == 0xe0 || st == 0xd0)
        {
            idx.controls.push_back (e);
        }
    }
    // A note never released lasts until the last event of the track.
    const auto last = events.empty() ? 0 : events.back().sample;
    for (auto& s : idx.spans)
    {
        if (s.end == std::numeric_limits<std::int64_t>::max())
            s.end = std::max (last, s.start);
        idx.longestSpan = std::max (idx.longestSpan, s.end - s.start);
    }
    return idx;
}

} // namespace b4nd
