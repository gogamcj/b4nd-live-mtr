#pragma once

#include <cstdint>
#include <set>
#include <vector>

#include "core/Midi.h"

namespace b4nd
{

/**
    MIDI editor model (Phase 11, piano roll). A track's tick events are split into notes
    (Note On + its Note Off) and everything else (CC, program, pitch bend...), which the editor
    never touches and writes back unchanged. All edits are pure data operations here, so that
    they are tested without the UI.

    Notes carry an id that stays the same across edits, so a selection survives sorting.
*/
struct EditNote
{
    std::uint32_t id = 0;
    std::int64_t start = 0;    // ticks at midiPpq
    std::int64_t length = 0;   // ticks, >= minNoteTicks
    int channel = 0;           // 0-15
    int pitch = 60;            // 0-127
    int velocity = 100;        // 1-127
    std::int64_t end() const noexcept { return start + length; }
};

constexpr std::int64_t minNoteTicks = midiPpq / 32;   // a 128th note

using NoteSelection = std::set<std::uint32_t>;

class NoteSequence
{
public:
    /** Pairs each Note On with the next Note Off of the same channel and pitch (first in,
        first out). A Note On without a Note Off ends at the last event of the track (at least
        one beat long). Unpaired Note Offs are dropped. */
    static NoteSequence fromEvents (const std::vector<MidiTickEvent>& sortedEvents);

    /** Notes and the other events again, sorted, Note Offs before Note Ons at the same tick.
        Overlapping notes of the same channel and pitch are cut where the next one starts
        (two Note Ons of one key before a Note Off would end both at the first Note Off on
        most instruments). */
    std::vector<MidiTickEvent> toEvents() const;

    const std::vector<EditNote>& getNotes() const noexcept { return notes; }
    const std::vector<MidiTickEvent>& getOtherEvents() const noexcept { return other; }
    const EditNote* find (std::uint32_t id) const noexcept;
    int size() const noexcept { return (int) notes.size(); }

    /** Adds a note (clamped to valid values). Returns its id. */
    std::uint32_t add (EditNote);
    /** Removes the selected notes. Returns how many. */
    int remove (const NoteSelection&);
    /** Moves the selection by ticks and semitones. The whole selection moves together: when a
        note would leave 0..127 or go before tick 0, the move is reduced for all of them. */
    void move (const NoteSelection&, std::int64_t dTicks, int dSemitones);
    /** Changes the length of the selected notes (each at least minNoteTicks). */
    void resize (const NoteSelection&, std::int64_t dTicks);
    /** Adds to the velocity of the selected notes (1..127). */
    void changeVelocity (const NoteSelection&, int delta);
    void setVelocity (const NoteSelection&, int velocity);
    /** Moves note starts towards the nearest grid line (grid in ticks, strength 0..1). Lengths
        are kept; with `ends` the end is quantized too (never shorter than minNoteTicks). */
    void quantize (const NoteSelection&, std::int64_t grid, double strength = 1.0, bool ends = false);

    /** Ids of the notes that start in [t0, t1) with pitch in [p0, p1] (rubber band selection). */
    NoteSelection notesIn (std::int64_t t0, std::int64_t t1, int p0, int p1) const;
    /** The note under a point: the top-most (last drawn) one. 0 when none. */
    std::uint32_t noteAt (std::int64_t tick, int pitch) const;
    NoteSelection all() const;

    std::int64_t lastTick() const noexcept;
    bool operator== (const NoteSequence& o) const;

private:
    void sort();
    std::vector<EditNote> notes;        // sorted by start, then pitch
    std::vector<MidiTickEvent> other;   // non-note channel messages, unchanged
    std::uint32_t nextId = 1;
};

/** Undo / redo of whole snapshots (a song part has at most a few thousand notes: cheap). */
class NoteEditHistory
{
public:
    static constexpr int maxSteps = 200;

    void reset (const NoteSequence& initial);
    /** Call after each edit with the new state. */
    void push (const NoteSequence& now);
    bool canUndo() const noexcept { return pos > 0; }
    bool canRedo() const noexcept { return pos + 1 < (int) steps.size(); }
    const NoteSequence& undo();
    const NoteSequence& redo();
    const NoteSequence& current() const { return steps[(size_t) pos]; }
    /** True when the current state differs from the first one (unsaved edits). */
    bool isModified() const { return ! steps.empty() && ! (steps[(size_t) pos] == base); }
    /** The current state is now the saved one. */
    void markSaved() { base = current(); }

private:
    std::vector<NoteSequence> steps;
    NoteSequence base;
    int pos = 0;
};

} // namespace b4nd
