#pragma once

#include <juce_core/juce_core.h>

namespace b4nd::systemguard
{

/** Keeps the computer from sleeping or turning the display off while a show runs: Windows
    SetThreadExecutionState, macOS an IOKit power assertion. Call on the message thread. No-op
    elsewhere. Returns false when not supported. */
bool keepAwake (bool on);
bool isKeepingAwake();

/** Memory of this process in bytes (0 when unknown): private bytes on Windows,
    physical footprint on macOS, resident set on Linux. */
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

/** macOS: 1 when the computer runs on its battery, 0 on the power adapter, -1 unknown
    (desktop Macs, other systems). */
int onBatteryPower();

} // namespace b4nd::systemguard
