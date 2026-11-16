#include "TestHarness.h"
#include "core/SpscQueue.h"

#include <cstdint>
#include <thread>

using namespace b4nd;

TEST_CASE ("SpscQueue: FIFO order, full and empty")
{
    SpscQueue<int, 4> q;
    int v = 0;
    CHECK (! q.pop (v));
    CHECK (q.push (1) && q.push (2) && q.push (3) && q.push (4));
    CHECK (! q.push (5));   // full: dropped, never blocks
    CHECK (q.pop (v) && v == 1);
    CHECK (q.push (5));
    CHECK (q.pop (v) && v == 2);
    CHECK (q.pop (v) && v == 3);
    CHECK (q.pop (v) && v == 4);
    CHECK (q.pop (v) && v == 5);
    CHECK (! q.pop (v));
}

TEST_CASE ("SpscQueue: two-thread stress keeps order and loses nothing")
{
    static SpscQueue<std::uint64_t, 1024> q;
    static constexpr std::uint64_t total = 2'000'000;

    std::thread producer ([] {
        for (std::uint64_t i = 1; i <= total; )
            if (q.push (i))
                ++i;
    });

    std::uint64_t expected = 1, v = 0;
    bool ordered = true;
    while (expected <= total)
    {
        if (q.pop (v))
        {
            if (v != expected) ordered = false;
            ++expected;
        }
    }
    producer.join();
    CHECK (ordered);
    CHECK (expected == total + 1);
}
