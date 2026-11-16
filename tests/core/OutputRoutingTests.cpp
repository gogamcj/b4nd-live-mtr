#include "TestHarness.h"
#include "core/OutputRouting.h"

using namespace b4nd;

TEST_CASE ("OutputRouting: default 8-out map matches spec")
{
    auto r = OutputRouting::createDefault (8);
    CHECK (r.getNumOutputs() == 8);
    CHECK (r.getOutput (0).isPA && r.getOutput (1).isPA);
    CHECK (r.getOutput (2).source->kind == BusKind::Click);
    CHECK (r.getOutput (3).source->kind == BusKind::Guide);
    CHECK (r.getOutput (4).source->kind == BusKind::Extra);
    CHECK (r.getOutput (6).source->kind == BusKind::Monitor);
    for (int i = 2; i < 8; ++i)
        CHECK (! r.getOutput (i).isPA);
    CHECK (r.isSafe());
}

TEST_CASE ("OutputRouting: click/guide/monitor can never be assigned to a PA output")
{
    const BusKind inEar[] = { BusKind::Click, BusKind::Guide, BusKind::Monitor };
    for (auto k : inEar)
    {
        auto r = OutputRouting::createDefault (8);
        CHECK (r.assign (0, { k, 0 }) == RoutingResult::RejectedInEarBusToPA);
        CHECK (r.getOutput (0).source->kind == BusKind::Main);   // unchanged
        CHECK (r.isSafe());
    }
}

TEST_CASE ("OutputRouting: marking a click output as PA is rejected")
{
    auto r = OutputRouting::createDefault (8);
    CHECK (r.setPA (2, true) == RoutingResult::RejectedPAWithInEarBus);
    CHECK (! r.getOutput (2).isPA);
    CHECK (r.isSafe());
}

TEST_CASE ("OutputRouting: deliberate click to OUT1 requires removing PA flag first")
{
    auto r = OutputRouting::createDefault (8);
    CHECK (r.setPA (0, false) == RoutingResult::Ok);
    CHECK (r.assign (0, { BusKind::Click, 0 }) == RoutingResult::Ok);
    CHECK (r.isSafe());
}

TEST_CASE ("OutputRouting: locked (Live Mode) rejects every edit")
{
    auto r = OutputRouting::createDefault (8);
    r.setLocked (true);
    CHECK (r.assign (4, { BusKind::Main, 0 }) == RoutingResult::RejectedLocked);
    CHECK (r.clear (2) == RoutingResult::RejectedLocked);
    CHECK (r.setPA (5, true) == RoutingResult::RejectedLocked);
    CHECK (r.setLabel (5, "x") == RoutingResult::RejectedLocked);
    CHECK (r.getOutput (2).source.has_value());
    r.setLocked (false);
    CHECK (r.assign (4, { BusKind::Main, 0 }) == RoutingResult::Ok);
}

TEST_CASE ("OutputRouting: invalid outputs and bus channels")
{
    auto r = OutputRouting::createDefault (8);
    CHECK (r.assign (8, { BusKind::Main, 0 }) == RoutingResult::InvalidOutput);
    CHECK (r.assign (-1, { BusKind::Main, 0 }) == RoutingResult::InvalidOutput);
    CHECK (r.assign (4, { BusKind::Click, 1 }) == RoutingResult::InvalidBusChannel);   // click is mono
    CHECK (r.assign (4, { BusKind::Main, 3 }) == RoutingResult::InvalidBusChannel);
    CHECK (r.assign (4, { BusKind::Click, BusChannel::monoSum }) == RoutingResult::InvalidBusChannel);   // mono bus
    CHECK (r.assign (4, { BusKind::Main, BusChannel::monoSum }) == RoutingResult::Ok);
}

TEST_CASE ("OutputRouting: devices with fewer than 8 outputs")
{
    auto r = OutputRouting::createDefault (2);
    CHECK (r.getNumOutputs() == 2);
    CHECK (r.outputsFor ({ BusKind::Click, 0 }).empty());
    CHECK (r.isSafe());
}

TEST_CASE ("OutputRouting: one bus can feed several outputs")
{
    auto r = OutputRouting::createDefault (10);
    CHECK (r.assign (8, { BusKind::Main, 0 }) == RoutingResult::Ok);
    CHECK (r.assign (9, { BusKind::Main, 1 }) == RoutingResult::Ok);
    CHECK (r.outputsFor ({ BusKind::Main, 0 }).size() == 2);
}

TEST_CASE ("OutputRouting: exhaustive invariant over all single edits")
{
    const BusKind kinds[] = { BusKind::Main, BusKind::Click, BusKind::Guide, BusKind::Extra, BusKind::Monitor, BusKind::Aux };
    for (int out = 0; out < 8; ++out)
        for (auto k : kinds)
            for (int ch = 0; ch < 2; ++ch)
                for (int pa = 0; pa < 2; ++pa)
                {
                    auto r = OutputRouting::createDefault (8);
                    r.setPA (out, pa == 1);
                    r.assign (out, { k, ch });
                    r.setPA (out, pa == 0);
                    CHECK (r.isSafe());
                }
}

TEST_CASE ("OutputRouting: 2-output split preset (UR22C) keeps click off the PA")
{
    auto r = OutputRouting::createForDevice (2);
    CHECK (r.getNumOutputs() == 2);
    CHECK (r.getOutput (0).isPA);
    CHECK (r.getOutput (0).source->kind == BusKind::Main);
    CHECK (r.getOutput (0).source->channel == BusChannel::monoSum);
    CHECK (! r.getOutput (1).isPA);
    CHECK (r.getOutput (1).source->kind == BusKind::Monitor);
    CHECK (r.isSafe());
    CHECK (r.assign (0, { BusKind::Monitor, BusChannel::monoSum }) == RoutingResult::RejectedInEarBusToPA);
    CHECK (OutputRouting::createForDevice (8).getOutput (2).source->kind == BusKind::Click);
    CHECK (OutputRouting::createForDevice (1).getNumOutputs() == 1);
    CHECK (OutputRouting::createForDevice (1).isSafe());
}
