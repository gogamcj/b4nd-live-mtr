#include "TestHarness.h"
#include "core/Midi.h"

using namespace b4nd;

TEST_CASE ("Midi: ticks to samples follow the tempo map")
{
    TempoMap t;
    t.setTempoPoints ({ { 0.0, 120.0 }, { 4.0, 60.0 } });
    std::vector<MidiTickEvent> ev = {
        { 0, MidiBytes::make (0x90, 60, 100) },
        { 960, MidiBytes::make (0x80, 60, 0) },            // beat 1 = 0.5 s
        { 960 * 5, MidiBytes::make (0x90, 62, 100) },      // beat 5: 2 s + 1 s at 60 BPM = 3 s
    };
    auto s = toSampleEvents (ev, t, 48000.0);
    CHECK (s.size() == 3);
    CHECK (s[0].sample == 0);
    CHECK (s[1].sample == 24000);
    CHECK (s[2].sample == 144000);
}

TEST_CASE ("Midi: note-off sorts before note-on at the same sample, channel override")
{
    TempoMap t;
    std::vector<MidiTickEvent> ev = {
        { 960, MidiBytes::make (0x93, 60, 90) },
        { 960, MidiBytes::make (0x83, 60, 0) },
        { 960, MidiBytes::make (0xb3, 7, 100) },
        { 0, MidiBytes::make (0xf0, 0, 0) },   // not a channel message: dropped
    };
    auto s = toSampleEvents (ev, t, 48000.0, 9);
    CHECK (s.size() == 3);
    CHECK (s[0].msg.isNoteOff());
    CHECK (s[1].msg.isController (7));
    CHECK (s[2].msg.isNoteOn());
    CHECK (s[2].msg.channel() == 9);
}

TEST_CASE ("Midi: active note tracker releases everything it started")
{
    ActiveNoteTracker tr;
    tr.observe (MidiBytes::make (0x90, 60, 100));
    tr.observe (MidiBytes::make (0x90, 64, 100));
    tr.observe (MidiBytes::make (0x91, 40, 100));
    tr.observe (MidiBytes::make (0x90, 64, 0));    // note-on velocity 0 = off
    tr.observe (MidiBytes::make (0xb1, 64, 127));  // sustain down
    CHECK (tr.activeCount() == 2);
    CHECK (tr.isNoteOn (0, 60) && ! tr.isNoteOn (0, 64) && tr.isNoteOn (1, 40));

    std::vector<MidiBytes> sent;
    tr.releaseAll ([&] (const MidiBytes& m) { sent.push_back (m); });
    // ch1: off 60, CC64 0, CC123 ; ch2: off 40, CC64 0, CC123
    CHECK (sent.size() == 6);
    CHECK (sent[0].isNoteOff() && sent[0].data[1] == 60);
    CHECK (sent[1].isController (64) && sent[1].data[2] == 0);
    CHECK (sent[2].isController (123));
    CHECK (sent[3].isNoteOff() && sent[3].channel() == 1);
    CHECK (! tr.anyActive());
}

TEST_CASE ("Midi: preset recall order, gaps and settle time")
{
    ModuleProfile p;
    p.channel = 3;
    p.bankMsb = 87; p.bankLsb = 64; p.program = 12;
    p.volume = 100; p.pan = 64;
    p.gapMs = 20; p.settleMs = 200;
    double ready = 0.0;
    auto seq = presetRecallSequence (p, &ready);
    CHECK (seq.size() == 5);
    CHECK (seq[0].msg.isController (0) && seq[0].msg.data[2] == 87 && seq[0].msg.channel() == 2);
    CHECK (seq[1].msg.isController (32) && seq[1].atMs == 20.0);
    CHECK (seq[2].msg.status() == 0xc0 && seq[2].msg.size == 2 && seq[2].msg.data[1] == 12 && seq[2].atMs == 40.0);
    CHECK (seq[3].msg.isController (7));
    CHECK (seq[4].msg.isController (10) && seq[4].atMs == 80.0);
    CHECK (ready == 280.0);

    p.sendOrder = "lsb, msb ,pc";
    p.bankMsb = -1;   // module without MSB
    p.volume = -1; p.pan = -1;
    seq = presetRecallSequence (p, &ready);
    CHECK (seq.size() == 2);
    CHECK (seq[0].msg.isController (32) && seq[0].atMs == 0.0);
    CHECK (seq[1].msg.status() == 0xc0 && seq[1].atMs == 20.0);
    CHECK (ready == 220.0);
}
