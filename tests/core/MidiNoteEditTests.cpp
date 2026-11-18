#include "TestHarness.h"
#include "core/MidiNoteEdit.h"

#include <algorithm>

using namespace b4nd;

namespace
{
MidiTickEvent on (std::int64_t t, int p, int v = 100, int ch = 0) { return { t, MidiBytes::make ((std::uint8_t) (0x90 | ch), (std::uint8_t) p, (std::uint8_t) v) }; }
MidiTickEvent off (std::int64_t t, int p, int ch = 0) { return { t, MidiBytes::make ((std::uint8_t) (0x80 | ch), (std::uint8_t) p, 0) }; }

int countOn (const std::vector<MidiTickEvent>& ev)
{
    int n = 0;
    for (const auto& e : ev)
        n += e.msg.isNoteOn() ? 1 : 0;
    return n;
}

/** Every Note On has a later Note Off of the same key and no key is struck twice while held. */
bool balanced (const std::vector<MidiTickEvent>& ev)
{
    int held[16][128] = {};
    std::int64_t last = -1;
    for (const auto& e : ev)
    {
        if (e.tick < last)
            return false;
        last = e.tick;
        auto& h = held[e.msg.channel()][e.msg.data[1] & 0x7f];
        if (e.msg.isNoteOn())
        {
            if (h != 0)
                return false;
            h = 1;
        }
        else if (e.msg.isNoteOff())
        {
            if (h == 0)
                return false;
            h = 0;
        }
    }
    for (auto& ch : held)
        for (int v : ch)
            if (v != 0)
                return false;
    return true;
}
} // namespace

TEST_CASE ("NoteEdit: events to notes and back keep notes and controllers")
{
    std::vector<MidiTickEvent> ev = {
        { 0, MidiBytes::make (0xc0, 5) },                  // program change: kept as is
        on (0, 60), on (0, 64, 80), off (480, 60), off (960, 64),
        { 480, MidiBytes::make (0xb0, 64, 127) },          // sustain
        on (960, 60, 90, 9), off (1200, 60, 9),            // drums channel
        off (2000, 70),                                    // stray Note Off: dropped
        on (1920, 72),                                     // never released: ends at the last event
    };
    std::stable_sort (ev.begin(), ev.end(), [] (auto& a, auto& b) { return a.tick < b.tick; });
    auto s = NoteSequence::fromEvents (ev);
    CHECK (s.size() == 4);
    CHECK (s.getOtherEvents().size() == 2);
    CHECK (s.getNotes()[0].pitch == 60 && s.getNotes()[0].length == 480);
    CHECK (s.getNotes()[1].pitch == 64 && s.getNotes()[1].length == 960 && s.getNotes()[1].velocity == 80);
    CHECK (s.getNotes()[2].channel == 9);
    CHECK (s.getNotes()[3].pitch == 72 && s.getNotes()[3].length == midiPpq);   // at least one beat

    auto back = s.toEvents();
    CHECK (balanced (back));
    CHECK (countOn (back) == 4);
    CHECK (back.front().msg.status() == 0xc0);   // program before the first notes at tick 0
    auto again = NoteSequence::fromEvents (back);
    CHECK (again == s);
}

TEST_CASE ("NoteEdit: move keeps the selection together inside the key and time range")
{
    auto s = NoteSequence::fromEvents ({ on (100, 120), on (200, 10), off (300, 120), off (400, 10) });
    auto sel = s.all();
    s.move (sel, -500, +20);   // can only go 100 ticks earlier and 7 semitones up
    CHECK (s.getNotes()[0].start == 0 && s.getNotes()[0].pitch == 127);
    CHECK (s.getNotes()[1].start == 100 && s.getNotes()[1].pitch == 17);
    const auto id = s.getNotes()[1].id;
    s.move ({ id }, -100, 0);
    CHECK (s.find (id)->start == 0);
    CHECK (s.find (id)->length == 200);   // moving never changes the length
}

TEST_CASE ("NoteEdit: quantize, resize, velocity, add and remove")
{
    auto s = NoteSequence::fromEvents ({ on (250, 60), off (700, 60), on (470, 62), off (500, 62) });
    auto sel = s.all();
    s.quantize (sel, 240);   // 16th notes at 960 PPQ
    CHECK (s.getNotes()[0].start == 240 && s.getNotes()[0].length == 450);
    CHECK (s.getNotes()[1].start == 480 && s.getNotes()[1].length == 30);

    auto h = NoteSequence::fromEvents ({ on (150, 60), off (350, 60) });
    h.quantize (h.all(), 240, 0.5);
    CHECK (h.getNotes()[0].start == 195);   // half way to 240
    h.quantize (h.all(), 240, 1.0, true);
    CHECK (h.getNotes()[0].start == 240 && h.getNotes()[0].length == 240);

    s.resize (sel, -1000);
    CHECK (s.getNotes()[0].length == minNoteTicks);
    s.changeVelocity (sel, 100);
    CHECK (s.getNotes()[0].velocity == 127);
    s.setVelocity (sel, 0);
    CHECK (s.getNotes()[0].velocity == 1);

    EditNote n;
    n.start = -50;
    n.length = 0;
    n.pitch = 200;
    const auto id = s.add (n);
    CHECK (s.find (id) != nullptr && s.find (id)->start == 0 && s.find (id)->pitch == 127 && s.find (id)->length == minNoteTicks);
    CHECK (s.getNotes().front().id == id);   // sorted by start
    CHECK (s.remove ({ id }) == 1);
    CHECK (s.size() == 2);
}

TEST_CASE ("NoteEdit: overlapping notes of one key never leave a note hanging")
{
    // Moved on top of each other: same key overlaps, and the exact same start.
    auto s = NoteSequence::fromEvents ({ on (0, 60), off (960, 60), on (480, 60, 50), off (1440, 60), on (2000, 64), off (2500, 64), on (2000, 64, 120), off (2100, 64) });
    CHECK (s.size() == 4);
    auto ev = s.toEvents();
    CHECK (balanced (ev));
    CHECK (countOn (ev) == 3);   // the two at 2000 became one note (longer, louder)
    auto back = NoteSequence::fromEvents (ev);
    CHECK (back.getNotes()[0].length == 480);   // cut where the next one starts
    CHECK (back.getNotes()[2].length == 500 && back.getNotes()[2].velocity == 120);
}

TEST_CASE ("NoteEdit: selection by area and by point")
{
    auto s = NoteSequence::fromEvents ({ on (0, 60), off (480, 60), on (480, 64), off (960, 64), on (960, 67), off (1440, 67) });
    CHECK (s.notesIn (400, 1000, 60, 66).size() == 2);   // 60 (still sounding at 400) and 64
    CHECK (s.notesIn (1000, 400, 66, 60).size() == 2);   // corners in any order
    CHECK (s.noteAt (500, 64) == s.getNotes()[1].id);
    CHECK (s.noteAt (500, 65) == 0);
}

TEST_CASE ("NoteEdit: undo and redo, modified flag")
{
    auto s = NoteSequence::fromEvents ({ on (0, 60), off (480, 60) });
    NoteEditHistory h;
    h.reset (s);
    CHECK (! h.isModified() && ! h.canUndo());
    s.move (s.all(), 0, 2);
    h.push (s);
    s.move (s.all(), 0, 2);
    h.push (s);
    h.push (s);   // no change: no extra step
    CHECK (h.isModified());
    CHECK (h.undo().getNotes()[0].pitch == 62);
    CHECK (h.undo().getNotes()[0].pitch == 60);
    CHECK (! h.isModified() && ! h.canUndo() && h.canRedo());
    CHECK (h.redo().getNotes()[0].pitch == 62);
    auto x = h.current();
    x.remove (x.all());
    h.push (x);   // a new edit drops the redo branch
    CHECK (! h.canRedo());
    h.markSaved();
    CHECK (! h.isModified());
    CHECK (h.undo().size() == 1 && h.isModified());
}
