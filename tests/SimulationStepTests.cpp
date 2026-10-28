#include "domain/SimulationStep.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Step headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>

using namespace hps::domain;
namespace
{
constexpr PlaceId input{1}, output{2};
constexpr TransitionId transition{3};
struct Net
{
    NetSnapshot snapshot;
    ConflictTopology topology;
    Net()
    {
        snapshot.net.places = {{input, u"Input", 10, 4}, {output, u"Output", 10, 0}};
        snapshot.marking = {{input, {4, 7}}, {output, {0, 8}}};
        snapshot.net.transitions = {{transition, u"T"}};
        snapshot.transitionStates = {{transition, {0, 5, false, false}}};
        topology = {{input, {}}, {output, {}}, {transition, {}}};
        Arc(input, transition, 2);
        Arc(transition, output);
    }
    void Arc(NodeId from, NodeId to, TokenCount weight = 1)
    {
        const ArcId id{static_cast<std::uint32_t>(snapshot.net.arcs.size() + 10)};
        snapshot.net.arcs.push_back({id, u"Arc", from, to, ArcKind::Normal, weight});
        topology.at(from).outgoing.push_back(id);
        topology.at(to).incoming.push_back(id);
    }
};
SimulationStepPlan Step(const NetSnapshot& snapshot, const ConflictTopology& topology,
    const StepClock& clock = {}, const StepRandomInputs& random = {})
{
    const auto result = PlanLegacySimulationStep(snapshot, topology, clock, random);
    REQUIRE(std::holds_alternative<SimulationStepPlan>(result));
    return std::get<SimulationStepPlan>(result);
}
bool HasError(const Net& net, ValidationCode code, const StepRandomInputs& random = {},
    const StepClock& clock = {})
{
    const auto result = PlanLegacySimulationStep(net.snapshot, net.topology, clock, random);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    const auto& issues = std::get<std::vector<ValidationIssue>>(result);
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == code; });
}
}

TEST_CASE("Full step copies graph and composes marking statistics and flags", "[simulation-step]")
{
    Net net;
    const auto plan = Step(net.snapshot, net.topology);
    CHECK(plan.snapshotAfter.marking.at(input).tokens == 2);
    CHECK(plan.snapshotAfter.marking.at(input).producedTokens == 7);
    CHECK(plan.snapshotAfter.marking.at(output).tokens == 1);
    CHECK(plan.snapshotAfter.marking.at(output).producedTokens == 9);
    CHECK(plan.snapshotAfter.transitionStates.at(transition).consumedTokens == 7);
    CHECK(plan.snapshotAfter.transitionStates.at(transition).active);
    CHECK_FALSE(plan.snapshotAfter.transitionStates.at(transition).enabled);
    CHECK(plan.clockAfter.steps == 1);
    CHECK(plan.clockAfter.timeMs == 0);
    CHECK(plan.movedTokens);
    CHECK_FALSE(plan.stop.Locked());
    CHECK(plan.potentialConflicts.empty());
    CHECK(plan.selection.selectedInNetOrder == std::vector<TransitionId>{transition});
    CHECK(ValidateConflictTopology(plan.snapshotAfter, net.topology).empty());
    net.snapshot.net.places.front().name = u"Changed";
    net.snapshot.net.arcs.front().weight = 9;
    CHECK(plan.snapshotAfter.net.places.front().name == u"Input");
    CHECK(plan.snapshotAfter.net.places.front().initialTokens == 4);
    CHECK(plan.snapshotAfter.net.arcs.front().weight == 2);
    CHECK(net.snapshot.marking.at(input).tokens == 4);
    CHECK(net.snapshot.transitionStates.at(transition).consumedTokens == 5);
}
TEST_CASE("Repeated full steps consume their own independent previous results", "[simulation-step]")
{
    const Net net;
    auto plan = Step(net.snapshot, net.topology);
    plan = Step(plan.snapshotAfter, net.topology, plan.clockAfter);
    CHECK(plan.snapshotAfter.marking.at(input).tokens == 0);
    CHECK(plan.snapshotAfter.marking.at(output).tokens == 2);
    CHECK(plan.snapshotAfter.transitionStates.at(transition).consumedTokens == 9);
    CHECK(plan.clockAfter.steps == 2);
    plan = Step(plan.snapshotAfter, net.topology, plan.clockAfter);
    CHECK(plan.stop.deadlock);
    CHECK_FALSE(plan.snapshotAfter.transitionStates.at(transition).active);
    CHECK(plan.clockAfter.steps == 3);
    CHECK(net.snapshot.marking.at(input).tokens == 4);
}
TEST_CASE("Complete fixed-delay steps advance time then fire and reset", "[simulation-step]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::FixedDelay;
    net.snapshot.net.transitions.front().initialDelayMs = 2;
    net.snapshot.transitionStates.at(transition).remainingDelayMs = 2;
    auto plan = Step(net.snapshot, net.topology);
    CHECK(plan.clockAfter.timeMs == 1);
    CHECK(plan.snapshotAfter.transitionStates.at(transition).remainingDelayMs == 1);
    CHECK_FALSE(plan.movedTokens);
    plan = Step(plan.snapshotAfter, net.topology, plan.clockAfter);
    CHECK(plan.clockAfter.timeMs == 2);
    CHECK(plan.snapshotAfter.transitionStates.at(transition).remainingDelayMs == 0);
    plan = Step(plan.snapshotAfter, net.topology, plan.clockAfter);
    CHECK(plan.clockAfter.timeMs == 2);
    CHECK(plan.snapshotAfter.transitionStates.at(transition).remainingDelayMs == 2);
    CHECK(plan.snapshotAfter.marking.at(output).tokens == 1);
}
TEST_CASE("Complete step derives conflict order and applies explicit winner", "[simulation-step]")
{
    Net net;
    const TransitionId second{4};
    net.snapshot.net.transitions.push_back({second, u"Second"});
    net.snapshot.transitionStates.emplace(second, TransitionState{});
    net.topology.emplace(second, NodeAdjacency{});
    net.Arc(input, second);
    CHECK(HasError(net, ValidationCode::InvalidConflictChoice));
    const auto plan = Step(net.snapshot, net.topology, {}, {{1}, {}});
    CHECK(plan.potentialConflicts == std::vector<TransitionId>{transition, second});
    CHECK(plan.selection.selectedInNetOrder == std::vector<TransitionId>{second});
    REQUIRE(plan.selection.decisions.size() == 1);
    CHECK(plan.selection.decisions.front().rawArcIndex == 1);
    CHECK(plan.snapshotAfter.marking.at(input).tokens == 3);
    CHECK(plan.snapshotAfter.marking.at(output).tokens == 0);
}
TEST_CASE("Token failure returns no complete state and preserves input", "[simulation-step]")
{
    Net net;
    net.snapshot.transitionStates.at(transition).consumedTokens = std::numeric_limits<std::int32_t>::max();
    CHECK(HasError(net, ValidationCode::ArithmeticOverflow));
    CHECK(net.snapshot.marking.at(input).tokens == 4);
    CHECK(net.snapshot.marking.at(output).producedTokens == 8);
}
TEST_CASE("Late timing failure preserves marking statistics flags and clock", "[simulation-step]")
{
    Net net;
    StepRandomInputs random;
    StepClock clock;
    ValidationCode expected = ValidationCode::InvalidDelaySample;
    net.snapshot.net.transitions.front().mode = TimeMode::Uniform;
    net.snapshot.net.transitions.front().initialDelayMs = 2;
    net.snapshot.net.transitions.front().rangeMaxDelayMs = 7;
    SECTION("Missing sample") {}
    SECTION("Invalid sample") { random.delays.emplace(transition, 7); }
    SECTION("Invalid clock") { random.delays.emplace(transition, 2); clock.sampleTimeMs = 0; expected = ValidationCode::InvalidSimulationSettings; }
    CHECK(HasError(net, expected, random, clock));
    CHECK(net.snapshot.marking.at(input).tokens == 4);
    CHECK(net.snapshot.marking.at(output).producedTokens == 8);
    CHECK(net.snapshot.transitionStates.at(transition).consumedTokens == 5);
    CHECK_FALSE(net.snapshot.transitionStates.at(transition).active);
    CHECK(clock.steps == 0);
}
TEST_CASE("Unexpected random inputs are errors even without conflicts or stochastic firings", "[simulation-step]")
{
    const Net net;
    CHECK(HasError(net, ValidationCode::InvalidConflictChoice, {{0}, {}}));
    CHECK(HasError(net, ValidationCode::InvalidDelaySample, {{}, {{transition, 2}}}));
}
TEST_CASE("Full step preserves simultaneous stop reasons and strict limits", "[simulation-step]")
{
    Net net;
    net.snapshot.marking.at(input).tokens = 0;
    const auto equality = Step(net.snapshot, net.topology, StepClock{1, 0, 1, 1, 1});
    CHECK(equality.stop.deadlock);
    CHECK_FALSE(equality.stop.timeLimitReached);
    CHECK_FALSE(equality.stop.stepLimitReached);
    const auto exceeded = Step(net.snapshot, net.topology, StepClock{2, 0, 1, 1, 0});
    CHECK(exceeded.stop.deadlock);
    CHECK(exceeded.stop.timeLimitReached);
    CHECK(exceeded.stop.stepLimitReached);
    CHECK(exceeded.stop.Locked());
}
TEST_CASE("An empty net returns a complete counted deadlock state", "[simulation-step]")
{
    const auto plan = Step({}, {});
    CHECK(plan.snapshotAfter.net.transitions.empty());
    CHECK(plan.snapshotAfter.marking.empty());
    CHECK(plan.clockAfter.steps == 1);
    CHECK(plan.stop.deadlock);
    CHECK(plan.selection.decisions.empty());
}
TEST_CASE("Full step forwards topology errors before selecting or changing state", "[simulation-step]")
{
    Net net;
    net.topology.at(input).outgoing.clear();
    CHECK(HasError(net, ValidationCode::MissingArcReference));
    CHECK(net.snapshot.marking.at(input).tokens == 4);
}
TEST_CASE("Explicit stochastic samples replay across countdown and firing steps", "[simulation-step]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::Uniform;
    net.snapshot.net.transitions.front().initialDelayMs = 2;
    net.snapshot.net.transitions.front().rangeMaxDelayMs = 7;
    auto plan = Step(net.snapshot, net.topology, {}, {{}, {{transition, 2}}});
    CHECK(plan.snapshotAfter.transitionStates.at(transition).remainingDelayMs == 2);
    plan = Step(plan.snapshotAfter, net.topology, plan.clockAfter);
    plan = Step(plan.snapshotAfter, net.topology, plan.clockAfter);
    CHECK(plan.clockAfter.timeMs == 2);
    plan = Step(plan.snapshotAfter, net.topology, plan.clockAfter, {{}, {{transition, 4}}});
    CHECK(plan.snapshotAfter.transitionStates.at(transition).remainingDelayMs == 4);
    CHECK(plan.snapshotAfter.marking.at(input).tokens == 0);
    CHECK(plan.snapshotAfter.marking.at(output).tokens == 2);
}
