#pragma once

#include <cstddef>

namespace b4ndtest
{
/** Counts heap allocations made on the current thread while a guard is alive. */
struct AllocationGuard
{
    AllocationGuard();
    ~AllocationGuard();
    std::size_t count() const;
};
} // namespace b4ndtest
