#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace b4nd
{

/**
    Bounded single-producer / single-consumer lock-free queue.

    - No allocation after construction, no locks, wait-free push/pop.
    - Exactly one thread may push and exactly one (other) thread may pop.
    - Used for UI -> audio commands and audio -> UI events (docs/01 §4).
*/
template <typename T, std::size_t Capacity>
class SpscQueue
{
    static_assert (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert (std::is_trivially_copyable_v<T>, "Queue items must be trivially copyable (no allocation on copy)");

public:
    /** Producer side. Returns false (and drops the item) if the queue is full. */
    bool push (const T& item) noexcept
    {
        const auto w = writeIndex.load (std::memory_order_relaxed);
        const auto r = readIndex.load (std::memory_order_acquire);
        if (w - r >= Capacity)
            return false;
        slots[w & (Capacity - 1)] = item;
        writeIndex.store (w + 1, std::memory_order_release);
        return true;
    }

    /** Consumer side. Returns false if empty. */
    bool pop (T& out) noexcept
    {
        const auto r = readIndex.load (std::memory_order_relaxed);
        const auto w = writeIndex.load (std::memory_order_acquire);
        if (r == w)
            return false;
        out = slots[r & (Capacity - 1)];
        readIndex.store (r + 1, std::memory_order_release);
        return true;
    }

    std::size_t sizeApprox() const noexcept
    {
        return writeIndex.load (std::memory_order_acquire) - readIndex.load (std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    std::array<T, Capacity> slots {};
    alignas (64) std::atomic<std::size_t> writeIndex { 0 };
    alignas (64) std::atomic<std::size_t> readIndex { 0 };
};

} // namespace b4nd
