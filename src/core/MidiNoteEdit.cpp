#include "core/MidiNoteEdit.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

namespace b4nd
{

namespace
{
int clampInt (int v, int lo, int hi) { return std::min (hi, std::max (lo, v)); }

EditNote sanitise (EditNote n)
{
    n.start = std::max<std::int64_t> (0, n.start);
    n.length = std::max (minNoteTicks, n.length);
    n.channel = clampInt (n.channel, 0, 15);
    n.pitch = clampInt (n.pitch, 0, 127);
    n.velocity = clampInt (n.velocity, 1, 127);
    return n;
}
} // namespace

NoteSequence NoteSequence::fromEvents (const std::vector<MidiTickEvent>& events)
{
    NoteSequence s;
    std::map<int, std::deque<size_t>> open;   // channel * 128 + pitch -> indices of notes waiting for their Note Off
    std::int64_t last = 0;
    for (const auto& e : events)
        last = std::max (last, e.tick);

    for (const auto& e : events)
    {
        const int len = midiMessageLength (e.msg.data[0]);
        if (len == 0 || e.msg.size < len)
            continue;   // not a channel message
        if (e.msg.isNoteOn())
        {
            EditNote n;
            n.id = s.nextId++;
            n.start = e.tick;
            n.length = -1;   // open
            n.channel = e.msg.channel();
            n.pitch = e.msg.data[1] & 0x7f;
            n.velocity = e.msg.data[2] & 0x7f;
            open[n.channel * 128 + n.pitch].push_back (s.notes.size());
            s.notes.push_back (n);
        }
        else if (e.msg.isNoteOff())
        {
            auto& q = open[e.msg.channel() * 128 + (e.msg.data[1] & 0x7f)];
            if (q.empty())
                continue;   // stray Note Off
            auto& n = s.notes[q.front()];
            q.pop_front();
            n.length = e.tick - n.start;
        }
        else
        {
            s.other.push_back (e);
        }
    }
    for (auto& n : s.notes)
    {
        if (n.length < 0)
            n.length = std::max<std::int64_t> (last - n.start, midiPpq);
        n = sanitise (n);
    }
    s.sort();
    return s;
}

void NoteSequence::sort()
{
    std::stable_sort (notes.begin(), notes.end(), [] (const EditNote& a, const EditNote& b) {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
}

std::vector<MidiTickEvent> NoteSequence::toEvents() const
{
    // Cut overlaps of one key (notes are sorted by start).
    std::vector<EditNote> out;
    out.reserve (notes.size());
    std::map<int, size_t> lastOfKey;
    for (const auto& n : notes)
    {
        const int key = n.channel * 128 + n.pitch;
        if (auto it = lastOfKey.find (key); it != lastOfKey.end())
        {
            auto& prev = out[it->second];
            if (prev.end() > n.start)
            {
                if (n.start <= prev.start)
                {
                    // same start: one Note On is enough - keep the longer and louder one
                    prev.length = std::max (prev.length, n.length);
                    prev.velocity = std::max (prev.velocity, n.velocity);
                    continue;
                }
                prev.length = n.start - prev.start;
            }
        }
        lastOfKey[key] = out.size();
        out.push_back (n);
    }

    std::vector<MidiTickEvent> ev;
    ev.reserve (out.size() * 2 + other.size());
    // Order at one tick: Note Offs, then controllers / programs, then Note Ons.
    std::vector<std::pair<int, MidiTickEvent>> tagged;
    tagged.reserve (ev.capacity());
    for (const auto& n : out)
    {
        tagged.push_back ({ 2, { n.start, MidiBytes::make ((std::uint8_t) (0x90 | n.channel), (std::uint8_t) n.pitch, (std::uint8_t) n.velocity) } });
        tagged.push_back ({ 0, { n.end(), MidiBytes::make ((std::uint8_t) (0x80 | n.channel), (std::uint8_t) n.pitch, 0) } });
    }
    for (const auto& e : other)
        tagged.push_back ({ 1, e });
    std::stable_sort (tagged.begin(), tagged.end(), [] (const auto& a, const auto& b) {
        return a.second.tick != b.second.tick ? a.second.tick < b.second.tick : a.first < b.first;
    });
    for (const auto& t : tagged)
        ev.push_back (t.second);
    return ev;
}

const EditNote* NoteSequence::find (std::uint32_t id) const noexcept
{
    for (const auto& n : notes)
        if (n.id == id)
            return &n;
    return nullptr;
}

std::uint32_t NoteSequence::add (EditNote n)
{
    n = sanitise (n);
    n.id = nextId++;
    notes.push_back (n);
    sort();
    return n.id;
}

int NoteSequence::remove (const NoteSelection& sel)
{
    const auto before = notes.size();
    notes.erase (std::remove_if (notes.begin(), notes.end(), [&] (const EditNote& n) { return sel.count (n.id) > 0; }), notes.end());
    return (int) (before - notes.size());
}

void NoteSequence::move (const NoteSelection& sel, std::int64_t dTicks, int dSemitones)
{
    std::int64_t minStart = INT64_MAX;
    int lo = 127, hi = 0;
    bool any = false;
    for (const auto& n : notes)
        if (sel.count (n.id))
        {
            any = true;
            minStart = std::min (minStart, n.start);
            lo = std::min (lo, n.pitch);
            hi = std::max (hi, n.pitch);
        }
    if (! any)
        return;
    dTicks = std::max (dTicks, -minStart);
    dSemitones = clampInt (dSemitones, -lo, 127 - hi);
    for (auto& n : notes)
        if (sel.count (n.id))
        {
            n.start += dTicks;
            n.pitch += dSemitones;
        }
    sort();
}

void NoteSequence::resize (const NoteSelection& sel, std::int64_t dTicks)
{
    for (auto& n : notes)
        if (sel.count (n.id))
            n.length = std::max (minNoteTicks, n.length + dTicks);
}

void NoteSequence::changeVelocity (const NoteSelection& sel, int delta)
{
    for (auto& n : notes)
        if (sel.count (n.id))
            n.velocity = clampInt (n.velocity + delta, 1, 127);
}

void NoteSequence::setVelocity (const NoteSelection& sel, int velocity)
{
    for (auto& n : notes)
        if (sel.count (n.id))
            n.velocity = clampInt (velocity, 1, 127);
}

void NoteSequence::quantize (const NoteSelection& sel, std::int64_t grid, double strength, bool ends)
{
    if (grid <= 0)
        return;
    strength = std::min (1.0, std::max (0.0, strength));
    auto snap = [&] (std::int64_t t) {
        const auto target = (std::int64_t) std::llround ((double) t / (double) grid) * grid;
        return t + (std::int64_t) std::llround ((double) (target - t) * strength);
    };
    for (auto& n : notes)
        if (sel.count (n.id))
        {
            const auto end = n.end();
            n.start = std::max<std::int64_t> (0, snap (n.start));
            if (ends)
                n.length = std::max (minNoteTicks, snap (end) - n.start);   // else the length is kept
        }
    sort();
}

NoteSelection NoteSequence::notesIn (std::int64_t t0, std::int64_t t1, int p0, int p1) const
{
    if (t1 < t0) std::swap (t0, t1);
    if (p1 < p0) std::swap (p0, p1);
    NoteSelection r;
    for (const auto& n : notes)
        if (n.pitch >= p0 && n.pitch <= p1 && n.end() > t0 && n.start < t1)
            r.insert (n.id);
    return r;
}

std::uint32_t NoteSequence::noteAt (std::int64_t tick, int pitch) const
{
    std::uint32_t id = 0;
    for (const auto& n : notes)
        if (n.pitch == pitch && tick >= n.start && tick < n.end())
            id = n.id;
    return id;
}

NoteSelection NoteSequence::all() const
{
    NoteSelection r;
    for (const auto& n : notes)
        r.insert (n.id);
    return r;
}

std::int64_t NoteSequence::lastTick() const noexcept
{
    std::int64_t t = 0;
    for (const auto& n : notes)
        t = std::max (t, n.end());
    for (const auto& e : other)
        t = std::max (t, e.tick);
    return t;
}

bool NoteSequence::operator== (const NoteSequence& o) const
{
    if (notes.size() != o.notes.size() || other.size() != o.other.size())
        return false;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& a = notes[i];
        const auto& b = o.notes[i];
        if (a.start != b.start || a.length != b.length || a.channel != b.channel || a.pitch != b.pitch || a.velocity != b.velocity)
            return false;
    }
    for (size_t i = 0; i < other.size(); ++i)
        if (other[i].tick != o.other[i].tick || other[i].msg.size != o.other[i].msg.size
            || other[i].msg.data[0] != o.other[i].msg.data[0] || other[i].msg.data[1] != o.other[i].msg.data[1]
            || other[i].msg.data[2] != o.other[i].msg.data[2])
            return false;
    return true;
}

//==============================================================================
void NoteEditHistory::reset (const NoteSequence& initial)
{
    steps.assign (1, initial);
    base = initial;
    pos = 0;
}

void NoteEditHistory::push (const NoteSequence& now)
{
    if (steps.empty())
    {
        reset (now);
        return;
    }
    steps.resize ((size_t) pos + 1);   // drop the redo branch
    if (steps.back() == now)
        return;
    steps.push_back (now);
    if ((int) steps.size() > maxSteps + 1)
        steps.erase (steps.begin());
    pos = (int) steps.size() - 1;
}

const NoteSequence& NoteEditHistory::undo()
{
    if (canUndo())
        --pos;
    return current();
}

const NoteSequence& NoteEditHistory::redo()
{
    if (canRedo())
        ++pos;
    return current();
}

} // namespace b4nd
