#include "app/SystemGuard.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 #include <powrprof.h>
 #include <vector>
 #pragma comment (lib, "psapi.lib")
 #pragma comment (lib, "powrprof.lib")
#elif JUCE_MAC
 #include <IOKit/pwr_mgt/IOPMLib.h>
 #include <IOKit/ps/IOPowerSources.h>
 #include <IOKit/ps/IOPSKeys.h>
 #include <mach/mach.h>
#elif JUCE_LINUX
 #include <unistd.h>
#endif

namespace b4nd::systemguard
{

static bool awake = false;
#if JUCE_MAC
static IOPMAssertionID sleepAssertion = kIOPMNullAssertionID;
#endif

bool keepAwake (bool on)
{
#if JUCE_WINDOWS
    const auto r = SetThreadExecutionState (on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
    awake = on && r != 0;
    return r != 0;
#elif JUCE_MAC
    // "PreventUserIdleDisplaySleep" keeps both the display and the system awake (lid open).
    if (on && sleepAssertion == kIOPMNullAssertionID)
    {
        const auto reason = CFSTR ("B4ND LIVE MTR: show running");
        if (IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                         reason, &sleepAssertion) != kIOReturnSuccess)
            sleepAssertion = kIOPMNullAssertionID;
    }
    else if (! on && sleepAssertion != kIOPMNullAssertionID)
    {
        IOPMAssertionRelease (sleepAssertion);
        sleepAssertion = kIOPMNullAssertionID;
    }
    awake = sleepAssertion != kIOPMNullAssertionID;
    return on ? awake : true;
#else
    awake = false;
    juce::ignoreUnused (on);
    return false;
#endif
}

bool isKeepingAwake() { return awake; }

std::int64_t residentBytes()
{
#if JUCE_WINDOWS
    // Private bytes, not the working set: Windows trims the working set (it went down by 50 MB in
    // a CI soak), which would hide a leak.
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    if (GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&pmc), sizeof (pmc)))
        return (std::int64_t) pmc.PrivateUsage;
    return 0;
#elif JUCE_LINUX
    const auto statm = juce::File ("/proc/self/statm").loadFileAsString();
    const auto parts = juce::StringArray::fromTokens (statm, " ", "");
    if (parts.size() < 2)
        return 0;
    return parts[1].getLargeIntValue() * (std::int64_t) juce::jmax (1L, sysconf (_SC_PAGESIZE));
#elif JUCE_MAC
    // The physical footprint is what Activity Monitor shows as "Memory": compressed and swapped
    // pages count too, so a leak cannot hide behind macOS memory compression.
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info (mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t> (&info), &count) == KERN_SUCCESS)
        return (std::int64_t) info.phys_footprint;
    return 0;
#else
    return 0;
#endif
}

PowerPlan activePowerPlan()
{
    PowerPlan plan;
#if JUCE_WINDOWS
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme (nullptr, &scheme) != ERROR_SUCCESS || scheme == nullptr)
        return plan;
    static const GUID highPerformance { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
    static const GUID ultimate { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
    plan.highPerformance = IsEqualGUID (*scheme, highPerformance) || IsEqualGUID (*scheme, ultimate);
    DWORD size = 0;
    if (PowerReadFriendlyName (nullptr, scheme, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size > 0)
    {
        std::vector<UCHAR> buffer (size + 2, 0);
        if (PowerReadFriendlyName (nullptr, scheme, nullptr, nullptr, buffer.data(), &size) == ERROR_SUCCESS)
            plan.name = juce::String (reinterpret_cast<const wchar_t*> (buffer.data()));
    }
    LocalFree (scheme);
    // OEM plans are often copies of High performance with another GUID: trust the name too.
    if (! plan.highPerformance)
        plan.highPerformance = plan.name.containsIgnoreCase ("high perf") || plan.name.containsIgnoreCase ("ultimate")
                               || plan.name.contains (juce::CharPointer_UTF8 ("\xea\xb3\xa0\xec\x84\xb1\xeb\x8a\xa5"));   // "고성능"
#endif
    return plan;
}

int onBatteryPower()
{
#if JUCE_MAC
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == nullptr)
        return -1;
    int result = -1;
    if (CFStringRef type = IOPSGetProvidingPowerSourceType (info); type != nullptr)
        result = CFStringCompare (type, CFSTR (kIOPMBatteryPowerKey), 0) == kCFCompareEqualTo ? 1 : 0;
    CFRelease (info);
    return result;
#else
    return -1;
#endif
}

} // namespace b4nd::systemguard
