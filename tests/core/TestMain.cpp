#include "TestHarness.h"

int main()
{
    int failedCases = 0;
    for (const auto& t : b4ndtest::registry())
    {
        const int before = b4ndtest::failures();
        t.fn();
        const bool ok = b4ndtest::failures() == before;
        std::printf ("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
        if (! ok) ++failedCases;
    }
    std::printf ("\n%d test cases, %d failed, %d failed checks\n",
                 static_cast<int> (b4ndtest::registry().size()), failedCases, b4ndtest::failures());
    return failedCases == 0 ? 0 : 1;
}
