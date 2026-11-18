// Locate chase (docs/04 §6): state at the jump point is recreated, bounded and in order.

#include "TestHarness.h"
#include "core/MidiChase.h"

#include <memory>
#include <vector>

using namespace b4nd;

namespace
{
MidiSampleEvent ev (std::int64_t s, std::uint8_t a, std::uint8_t b, std::uint8_t c = 0)
{
    return { s, (a & 0xf0) == 0xc0 || (a & 0xf0) == 0xd0 ? MidiBytes::make (a, b) : MidiBytes::make (a, b, c) };
}
std::vector<MidiBytes> run (const ChaseIndex& idx, std::int64_t pos, bool notes = true, std::int64_t minRemaining = 0)
{
    auto chaser = std::make_unique<MidiChaser>();
    std::vector<MidiBytes> out;
    chaser->chase (idx, pos, notes, minRemaining, [&] (const MidiBytes& m) { out.push_back (m); });
    return out;
}
bool same (const MidiBytes& m, std::uint8_t a, std::uint8_t b, std::uint8_t c = 0)
{
    return m.data[0] == a && m.data[1] == b && (m.size < 3 || m.data[2] == c);
}
} // namespace

TEST_CASE ("Chase: newest bank / program / controllers before the jump, in their original order")
{
    const auto idx = ChaseIndex::build ({
        ev (0, 0xb0, 0, 87), ev (0, 0xb0, 32, 64), ev (0, 0xc0, 12),     // bank 87/64, program 12
        ev (100, 0xb0, 7, 100), ev (200, 0xb0, 7, 80),                   // volume changes: 80 wins
        ev (300, 0xb0, 0, 86), ev (300, 0xc0, 5),                        // later preset change
        ev (400, 0xe0, 0, 80), ev (500, 0xb1, 1, 30),                    // pitch bend, mod on ch 2
        ev (900, 0xb0, 7, 10),                                           // after the jump: not sent
        ev (950, 0xb0, 121, 0),                                          // reset controllers: never chased
    });
    const auto m = run (idx, 600);
    CHECK (m.size() == 6);
    if (m.size() != 6) return;
    // Oldest first: CC32 64 (t=0), CC7 80 (t=200), CC0 86 + PC 5 (t=300), bend (400), ch2 mod (500)
    CHECK (same (m[0], 0xb0, 32, 64));
    CHECK (same (m[1], 0xb0, 7, 80));
    CHECK (same (m[2], 0xb0, 0, 86));
    CHECK (same (m[3], 0xc0, 5));
    CHECK (same (m[4], 0xe0, 0, 80));
    CHECK (same (m[5], 0xb1, 1, 30));
    CHECK (run (idx, 0).empty());   // nothing before the start
}

TEST_CASE ("Chase: only notes still held at the jump, with enough time left")
{
    const auto idx = ChaseIndex::build ({
        ev (0, 0x90, 48, 90), ev (48000, 0x80, 48, 0),          // pad 0-1 s
        ev (1000, 0x90, 60, 100), ev (2000, 0x80, 60, 0),       // short note, over before the jump
        ev (10000, 0x91, 64, 70), ev (12000, 0x81, 64, 0),      // ends 2000 samples after the jump
        ev (10000, 0x90, 67, 80),                               // never released: lasts to the end
        ev (60000, 0xb0, 1, 0),
    });
    auto m = run (idx, 10000 + 0);   // note at exactly pos plays normally, not chased
    CHECK (m.size() == 1 && m[0].data[1] == 48);
    m = run (idx, 11000);
    CHECK (m.size() == 3);
    m = run (idx, 11000, true, 4800);   // at least 0.1 s left: the ch2 note (1000 left) is skipped
    CHECK (m.size() == 2);
    m = run (idx, 11000, false);
    CHECK (m.empty());
}

TEST_CASE ("Chase: work is bounded on huge controller streams")
{
    std::vector<MidiSampleEvent> e;
    for (int i = 0; i < 100000; ++i)
        e.push_back (ev (i, 0xb0, (std::uint8_t) (i % 2 == 0 ? 1 : 11), (std::uint8_t) (i % 128)));   // two controllers, dense
    const auto idx = ChaseIndex::build (e);
    const auto m = run (idx, 99999);
    CHECK (m.size() == 2);   // newest of each, found quickly
}
