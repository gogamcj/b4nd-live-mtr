#include "core/OutputRouting.h"

namespace b4nd
{

const char* busKindName (BusKind k) noexcept
{
    switch (k)
    {
        case BusKind::Main:    return "Main";
        case BusKind::Click:   return "Click";
        case BusKind::Guide:   return "Guide";
        case BusKind::Extra:   return "Extra";
        case BusKind::Monitor: return "Monitor";
        case BusKind::Aux:     return "Aux";
    }
    return "?";
}

const char* routingResultMessage (RoutingResult r) noexcept
{
    switch (r)
    {
        case RoutingResult::Ok:                     return "OK";
        case RoutingResult::InvalidOutput:          return "Output does not exist on this device";
        case RoutingResult::InvalidBusChannel:      return "Bus channel does not exist";
        case RoutingResult::RejectedInEarBusToPA:   return "Click/Guide/Monitor cannot be routed to a PA output. Remove the PA flag first.";
        case RoutingResult::RejectedPAWithInEarBus: return "This output carries Click/Guide/Monitor and cannot be marked as PA.";
        case RoutingResult::RejectedLocked:         return "Routing is locked in Live Mode";
    }
    return "?";
}

int OutputRouting::channelsForBus (BusKind k) noexcept
{
    return (k == BusKind::Click || k == BusKind::Guide) ? 1 : 2;
}

OutputRouting::OutputRouting (int numPhysicalOutputs)
    : outputs (static_cast<std::size_t> (numPhysicalOutputs > 0 ? numPhysicalOutputs : 0))
{
    for (int i = 0; i < getNumOutputs(); ++i)
        outputs[static_cast<std::size_t> (i)].label = "OUT " + std::to_string (i + 1);
}

OutputRouting OutputRouting::createDefault (int numPhysicalOutputs)
{
    OutputRouting r (numPhysicalOutputs);

    struct Def { BusChannel src; bool pa; const char* label; };
    const Def defs[] = {
        { { BusKind::Main,    0 }, true,  "MAIN LEFT" },
        { { BusKind::Main,    1 }, true,  "MAIN RIGHT" },
        { { BusKind::Click,   0 }, false, "CLICK" },
        { { BusKind::Guide,   0 }, false, "GUIDE" },
        { { BusKind::Extra,   0 }, false, "EXTRA LEFT" },
        { { BusKind::Extra,   1 }, false, "EXTRA RIGHT" },
        { { BusKind::Monitor, 0 }, false, "MONITOR LEFT" },
        { { BusKind::Monitor, 1 }, false, "MONITOR RIGHT" },
    };

    for (int i = 0; i < r.getNumOutputs() && i < 8; ++i)
    {
        auto& o = r.outputs[static_cast<std::size_t> (i)];
        o.source = defs[i].src;
        o.isPA = defs[i].pa;
        o.label = defs[i].label;
    }
    return r;
}

bool OutputRouting::isValidChannel (BusKind k, int channel) noexcept
{
    const int n = channelsForBus (k);
    return (channel >= 0 && channel < n) || (n == 2 && channel == BusChannel::monoSum);
}

OutputRouting OutputRouting::createTwoOutputSplit()
{
    OutputRouting r (2);
    r.outputs[0].source = BusChannel { BusKind::Main, BusChannel::monoSum };
    r.outputs[0].isPA = true;
    r.outputs[0].label = "MAIN MONO";
    r.outputs[1].source = BusChannel { BusKind::Monitor, BusChannel::monoSum };
    r.outputs[1].isPA = false;
    r.outputs[1].label = "IN-EAR (CLICK+GUIDE)";
    return r;
}

OutputRouting OutputRouting::createForDevice (int numPhysicalOutputs)
{
    if (numPhysicalOutputs <= 0)
        return createDefault (8);
    if (numPhysicalOutputs < 4)
    {
        auto r = createTwoOutputSplit();
        if (numPhysicalOutputs == 1)
        {
            OutputRouting mono (1);
            mono.outputs[0] = r.outputs[0];
            return mono;
        }
        if (numPhysicalOutputs == 3)
        {
            OutputRouting three (3);
            three.outputs[0] = r.outputs[0];
            three.outputs[1] = r.outputs[1];
            three.outputs[2].label = "OUT 3";
            return three;
        }
        return r;
    }
    return createDefault (numPhysicalOutputs);
}

RoutingResult OutputRouting::assign (int output, BusChannel source)
{
    if (locked)                    return RoutingResult::RejectedLocked;
    if (! validOutput (output))    return RoutingResult::InvalidOutput;
    if (! isValidChannel (source.kind, source.channel))
        return RoutingResult::InvalidBusChannel;

    auto& o = outputs[static_cast<std::size_t> (output)];
    if (o.isPA && isInEarOnlyBus (source.kind))
        return RoutingResult::RejectedInEarBusToPA;

    o.source = source;
    return RoutingResult::Ok;
}

RoutingResult OutputRouting::clear (int output)
{
    if (locked)                 return RoutingResult::RejectedLocked;
    if (! validOutput (output)) return RoutingResult::InvalidOutput;
    outputs[static_cast<std::size_t> (output)].source.reset();
    return RoutingResult::Ok;
}

RoutingResult OutputRouting::setPA (int output, bool isPA)
{
    if (locked)                 return RoutingResult::RejectedLocked;
    if (! validOutput (output)) return RoutingResult::InvalidOutput;

    auto& o = outputs[static_cast<std::size_t> (output)];
    if (isPA && o.source && isInEarOnlyBus (o.source->kind))
        return RoutingResult::RejectedPAWithInEarBus;

    o.isPA = isPA;
    return RoutingResult::Ok;
}

RoutingResult OutputRouting::setLabel (int output, std::string label)
{
    if (locked)                 return RoutingResult::RejectedLocked;
    if (! validOutput (output)) return RoutingResult::InvalidOutput;
    outputs[static_cast<std::size_t> (output)].label = std::move (label);
    return RoutingResult::Ok;
}

int OutputRouting::sanitize()
{
    int cleared = 0;
    for (auto& o : outputs)
    {
        if (o.source && ! isValidChannel (o.source->kind, o.source->channel))
        {
            o.source.reset();
            ++cleared;
        }
        else if (o.isPA && o.source && isInEarOnlyBus (o.source->kind))
        {
            o.source.reset();
            ++cleared;
        }
    }
    return cleared;
}

bool OutputRouting::isSafe() const noexcept
{
    for (const auto& o : outputs)
        if (o.isPA && o.source && isInEarOnlyBus (o.source->kind))
            return false;
    return true;
}

std::vector<int> OutputRouting::outputsFor (BusChannel source) const
{
    std::vector<int> result;
    for (int i = 0; i < getNumOutputs(); ++i)
    {
        const auto& o = outputs[static_cast<std::size_t> (i)];
        if (o.source && *o.source == source)
            result.push_back (i);
    }
    return result;
}

} // namespace b4nd
