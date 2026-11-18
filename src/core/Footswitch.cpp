#include "core/Footswitch.h"

namespace b4nd
{

namespace
{
struct ActionName { LiveAction a; const char* id; const char* label; };
constexpr ActionName actionNames[] = {
    { LiveAction::None, "none", "-" },
    { LiveAction::PlayPause, "play_pause", "PLAY / PAUSE" },
    { LiveAction::Stop, "stop", "STOP" },
    { LiveAction::Restart, "restart", "RESTART SONG" },
    { LiveAction::NextSong, "next_song", "NEXT SONG" },
    { LiveAction::PreviousSong, "previous_song", "PREVIOUS SONG" },
    { LiveAction::NextCue, "next_cue", "NEXT CUE" },
    { LiveAction::PreviousCue, "previous_cue", "PREVIOUS CUE" },
    { LiveAction::ToggleLoop, "toggle_loop", "LOOP ON / OFF" },
    { LiveAction::EmergencyStop, "emergency_stop", "EMERGENCY STOP" },
};
} // namespace

const char* liveActionId (LiveAction a) noexcept
{
    for (const auto& n : actionNames)
        if (n.a == a)
            return n.id;
    return "none";
}

const char* liveActionLabel (LiveAction a) noexcept
{
    for (const auto& n : actionNames)
        if (n.a == a)
            return n.label;
    return "-";
}

LiveAction liveActionFromId (const std::string& id) noexcept
{
    for (const auto& n : actionNames)
        if (id == n.id)
            return n.a;
    return LiveAction::None;
}

std::vector<FootswitchBinding> FootswitchMapper::defaultBindings()
{
    // docs/04 §8. Emergency stop needs a double press so that a stray step never stops a song.
    auto cc = [] (int n, LiveAction a, bool dbl = false) { FootswitchBinding b; b.number = n; b.action = a; b.doublePress = dbl; return b; };
    auto note = [] (int n, LiveAction a) { FootswitchBinding b; b.note = true; b.number = n; b.action = a; return b; };
    return { cc (64, LiveAction::PlayPause), note (60, LiveAction::PlayPause),
             cc (65, LiveAction::NextSong), note (62, LiveAction::NextSong),
             cc (66, LiveAction::PreviousSong), note (64, LiveAction::PreviousSong),
             cc (67, LiveAction::NextCue),
             cc (68, LiveAction::EmergencyStop, true) };
}

void FootswitchMapper::setBindings (const std::vector<FootswitchBinding>& b)
{
    bindings.clear();
    for (const auto& x : b)
    {
        if ((int) bindings.size() >= maxBindings)
            break;
        if (x.action == LiveAction::None || x.number < 0 || x.number > 127 || x.channel < -1 || x.channel > 15)
            continue;
        bindings.push_back (x);
    }
    reset();
}

void FootswitchMapper::reset() noexcept
{
    for (auto& k : inputs)
        for (auto& s : k)
            s = InputState {};
}

int FootswitchMapper::findBinding (bool note, int channel, int number, const std::string& port, bool wantDouble) const noexcept
{
    for (int i = 0; i < (int) bindings.size(); ++i)
    {
        const auto& b = bindings[(size_t) i];
        if (b.note == note && b.number == number && (b.channel < 0 || b.channel == channel)
            && (b.port.empty() || b.port == port) && b.doublePress == wantDouble)
            return i;
    }
    return -1;
}

FootswitchMapper::Result FootswitchMapper::handle (const MidiBytes& m, const std::string& port, double nowMs) noexcept
{
    Result r;
    const int status = m.status();
    const bool isNote = status == 0x90 || status == 0x80;
    const bool isCc = status == 0xb0;
    if ((! isNote && ! isCc) || m.size < 3)
        return r;
    const int ch = m.channel();
    const int num = m.data[1] & 0x7f;
    auto& st = inputs[(size_t) ((isNote ? 16 : 0) + ch)][(size_t) num];

    bool press = false;
    if (isNote)
        press = m.isNoteOn();
    else
    {
        const int value = m.data[2] & 0x7f;
        const bool down = value >= 64;
        bool latching = false;
        if (! learning)
            for (const auto& b : bindings)
                if (! b.note && b.number == num && (b.channel < 0 || b.channel == ch) && (b.port.empty() || b.port == port))
                    latching = latching || b.latching;
        if (latching)
            press = down != st.down;                                  // every change is a press
        else
            press = (down && ! st.down) || (down && value == st.lastValue);   // up movement, or a repeated trigger
        st.down = down;
        st.lastValue = value;
    }
    if (! press)
        return r;
    if (nowMs - st.lastPress < debounceMs)
        return r;   // contact bounce
    st.lastPress = nowMs;

    r.press = true;
    r.input.port = port;
    r.input.note = isNote;
    r.input.channel = ch;
    r.input.number = num;
    if (learning)
    {
        r.learned = true;
        return r;
    }

    if (const int d = findBinding (isNote, ch, num, port, true); d >= 0)
    {
        if (nowMs - st.pending <= doublePressMs)
        {
            st.pending = -1.0e9;
            r.action = bindings[(size_t) d].action;
        }
        else
        {
            st.pending = nowMs;
            r.waitingForSecond = true;
        }
        return r;   // a double-press input never also acts on one press
    }
    if (const int s = findBinding (isNote, ch, num, port, false); s >= 0)
        r.action = bindings[(size_t) s].action;
    return r;
}

} // namespace b4nd
