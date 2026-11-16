#include "AllocationGuard.h"

#include <cstdlib>
#include <new>

namespace
{
thread_local bool tracking = false;
thread_local std::size_t allocations = 0;
} // namespace

namespace b4ndtest
{
AllocationGuard::AllocationGuard()  { allocations = 0; tracking = true; }
AllocationGuard::~AllocationGuard() { tracking = false; }
std::size_t AllocationGuard::count() const { return allocations; }
} // namespace b4ndtest

void* operator new (std::size_t size)
{
    if (tracking)
        ++allocations;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    return ::operator new (size);
}

void operator delete (void* p) noexcept                      { std::free (p); }
void operator delete[] (void* p) noexcept                    { std::free (p); }
void operator delete (void* p, std::size_t) noexcept         { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept       { std::free (p); }
