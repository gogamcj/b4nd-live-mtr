#pragma once

#include <optional>
#include <string>
#include <vector>

namespace b4nd
{

enum class BusKind
{
    Main,
    Click,
    Guide,
    Extra,
    Monitor,
    Aux
};

const char* busKindName (BusKind) noexcept;

/** Busses that may carry click or cue signals and must never reach the PA. */
constexpr bool isInEarOnlyBus (BusKind k) noexcept
{
    return k == BusKind::Click || k == BusKind::Guide || k == BusKind::Monitor;
}

struct BusChannel
{
    BusKind kind = BusKind::Main;
    int channel = 0;   // 0 = left / mono, 1 = right

    bool operator== (const BusChannel& o) const noexcept { return kind == o.kind && channel == o.channel; }
};

struct PhysicalOutput
{
    std::optional<BusChannel> source;
    bool isPA = false;      // feeds the front-of-house PA: click/guide/monitor forbidden
    std::string label;
};

enum class RoutingResult
{
    Ok,
    InvalidOutput,
    InvalidBusChannel,
    RejectedInEarBusToPA,   // tried to send click/guide/monitor to a PA output
    RejectedPAWithInEarBus, // tried to flag an output as PA while it carries click/guide/monitor
    RejectedLocked          // routing edits are locked (Live Mode)
};

const char* routingResultMessage (RoutingResult) noexcept;

/**
    Physical output assignment table with click protection.

    Invariant (enforced by every mutating call): no output flagged isPA carries a
    Click, Guide or Monitor bus channel. Editing happens on the message thread;
    the audio thread receives an immutable copy (see docs/06-audio-routing.md).
*/
class OutputRouting
{
public:
    explicit OutputRouting (int numPhysicalOutputs = 8);

    /** OUT1/2 Main L/R (PA), OUT3 Click, OUT4 Guide, OUT5/6 Extra, OUT7/8 Monitor. */
    static OutputRouting createDefault (int numPhysicalOutputs = 8);

    static int channelsForBus (BusKind k) noexcept;

    int getNumOutputs() const noexcept { return static_cast<int> (outputs.size()); }
    const PhysicalOutput& getOutput (int index) const { return outputs.at (static_cast<std::size_t> (index)); }

    RoutingResult assign (int output, BusChannel source);
    RoutingResult clear (int output);
    RoutingResult setPA (int output, bool isPA);
    RoutingResult setLabel (int output, std::string label);

    /** Live Mode lock: all edits are rejected while locked. */
    void setLocked (bool shouldLock) noexcept { locked = shouldLock; }
    bool isLocked() const noexcept { return locked; }

    /** Removes any assignment that violates the click-protection invariant
        (e.g. from a hand-edited or old project file). Returns how many were cleared. */
    int sanitize();

    bool isSafe() const noexcept;

    /** Outputs that currently carry the given bus channel. */
    std::vector<int> outputsFor (BusChannel source) const;

private:
    bool validOutput (int index) const noexcept { return index >= 0 && index < getNumOutputs(); }

    std::vector<PhysicalOutput> outputs;
    bool locked = false;
};

} // namespace b4nd
