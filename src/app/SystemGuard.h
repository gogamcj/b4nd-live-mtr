#pragma once

#include <juce_core/juce_core.h>

namespace b4nd::systemguard
{

/** Keeps Windows from sleeping or turning the display off (SetThreadExecutionState), while a
    show runs. Call on the message thread. No-op elsewhere. Returns false when not supported. */
bool keepAwake (bool on);
bool isKeepingAwake();

/** Resident memory of this process in bytes (0 when unknown). */
std::int64_t residentBytes();

/** Windows: the active power plan, read through the power API (the name comes localised, e.g.
    "고성능", so the decision uses the plan's GUID). Empty name elsewhere or when it cannot be read.
    Call it rarely (show check), not in a loop. */
struct PowerPlan
{
    juce::String name;
    bool highPerformance = false;   // High performance or Ultimate performance
};
PowerPlan activePowerPlan();

} // namespace b4nd::systemguard
