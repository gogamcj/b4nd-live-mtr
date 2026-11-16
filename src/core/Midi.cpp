#include "core/Midi.h"

#include <algorithm>
#include <cmath>

namespace b4nd
{

int midiMessageLength (std::uint8_t status) noexcept
{
    switch (status & 0xf0)
    {
        case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0: return 3;
        case 0xc0: case 0xd0: return 2;
        default: return 0;
    }
}

std::vector<MidiSampleEvent> toSampleEvents (const std::vector<MidiTickEvent>& events, const TempoMap& tempo,
                                             double sampleRate, int forceChannel)
{
    std::vector<MidiSampleEvent> out;
    out.reserve (events.size());
    for (const auto& e : events)
    {
        if (e.msg.size == 0 || midiMessageLength (e.msg.data[0]) == 0)
            continue;
        MidiSampleEvent s;
        s.msg = e.msg;
        if (forceChannel >= 0 && forceChannel < 16)
            s.msg.data[0] = (std::uint8_t) ((s.msg.data[0] & 0xf0) | forceChannel);
        const double beat = (double) e.tick / (double) midiPpq;
        s.sample = (std::int64_t) std::llround (tempo.secondsAtBeat (beat) * sampleRate);
        out.push_back (s);
    }
    // Same sample: note-offs first, then controllers/programs, then note-ons.
    auto rank = [] (const MidiBytes& m) { return m.isNoteOff() ? 0 : m.isNoteOn() ? 2 : 1; };
    std::stable_sort (out.begin(), out.end(), [&] (const MidiSampleEvent& a, const MidiSampleEvent& b)
    {
        if (a.sample != b.sample)
            return a.sample < b.sample;
        return rank (a.msg) < rank (b.msg);
    });
    return out;
}

//==============================================================================
void ActiveNoteTracker::reset() noexcept
{
    for (auto& ch : notes)
        ch.fill (false);
    sustain.fill (false);
    usedChannels.fill (false);
}

void ActiveNoteTracker::observe (const MidiBytes& m) noexcept
{
    if (m.size == 0 || midiMessageLength (m.data[0]) == 0)
        return;
    const auto ch = (size_t) m.channel();
    usedChannels[ch] = true;
    if (m.isNoteOn())
        notes[ch][(size_t) (m.data[1] & 0x7f)] = true;
    else if (m.isNoteOff())
        notes[ch][(size_t) (m.data[1] & 0x7f)] = false;
    else if (m.isController (64))
        sustain[ch] = m.data[2] >= 64;
    else if (m.isController (123) || m.isController (120))
        notes[ch].fill (false);
}

bool ActiveNoteTracker::isNoteOn (int channel, int note) const noexcept
{
    return channel >= 0 && channel < 16 && note >= 0 && note < 128 && notes[(size_t) channel][(size_t) note];
}

int ActiveNoteTracker::activeCount() const noexcept
{
    int n = 0;
    for (const auto& ch : notes)
        for (bool b : ch)
            n += b ? 1 : 0;
    return n;
}

bool ActiveNoteTracker::anyActive() const noexcept
{
    return activeCount() > 0 || std::any_of (sustain.begin(), sustain.end(), [] (bool b) { return b; });
}

//==============================================================================
std::vector<TimedMidi> presetRecallSequence (const ModuleProfile& p, double* readyAtMs)
{
    std::vector<TimedMidi> out;
    const int ch = std::clamp (p.channel, 1, 16) - 1;
    const auto cc = [ch] (int num, int val) { return MidiBytes::make ((std::uint8_t) (0xb0 | ch), (std::uint8_t) (num & 0x7f), (std::uint8_t) (val & 0x7f)); };
    double t = 0.0;
    const double gap = std::max (0, p.gapMs);

    // Bank/program in the configured order (some older modules need LSB before MSB, or PC last only).
    std::vector<std::string> order;
    {
        std::string item;
        for (char c : p.sendOrder + ",")
        {
            if (c == ',') { if (! item.empty()) order.push_back (item); item.clear(); }
            else if (c != ' ') item += c;
        }
    }
    if (order.empty())
        order = { "msb", "lsb", "pc" };

    bool first = true;
    for (const auto& step : order)
    {
        MidiBytes m;
        if (step == "msb" && p.bankMsb >= 0)       m = cc (0, p.bankMsb);
        else if (step == "lsb" && p.bankLsb >= 0)  m = cc (32, p.bankLsb);
        else if (step == "pc" && p.program >= 0)   m = MidiBytes::make ((std::uint8_t) (0xc0 | ch), (std::uint8_t) (p.program & 0x7f));
        else continue;
        if (! first)
            t += gap;
        first = false;
        out.push_back ({ t, m });
    }
    if (p.volume >= 0)
    {
        t += first ? 0.0 : gap;
        first = false;
        out.push_back ({ t, cc (p.volumeCc, p.volume) });
    }
    if (p.pan >= 0)
    {
        t += first ? 0.0 : gap;
        first = false;
        out.push_back ({ t, cc (p.panCc, p.pan) });
    }
    if (readyAtMs != nullptr)
        *readyAtMs = t + std::max (0, p.settleMs);
    return out;
}

} // namespace b4nd
