#include "simulation/SimulationEngine.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Engine headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>

using namespace hps::domain;
using namespace hps::simulation;
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
        snapshot.net.places = {{input, u"Input", 10, 5}, {output, u"Output", 10, 0}};
        snapshot.marking = {{input, {2, 7}}, {output, {0, 8}}};
        snapshot.net.transitions = {{transition, u"T"}};
        snapshot.transitionStates = {{transition, {0, 9, false, false}}};
        snapshot.net.arcs = {{ArcId{4}, u"In", input, transition}, {ArcId{5}, u"Out", transition, output}};
        topology = {{input, {{}, {ArcId{4}}}}, {transition, {{ArcId{4}}, {ArcId{5}}}}, {output, {{ArcId{5}}, {}}}};
    }
};
SimulationEngine Engine(const Net& net, const StepClock& clock = {})
{
    auto result = SimulationEngine::Create(net.snapshot, net.topology, clock);
    REQUIRE(std::holds_alternative<SimulationEngine>(result));
    return std::get<SimulationEngine>(std::move(result));
}
void Same(const EngineSnapshot& a, const EngineSnapshot& b)
{
    CHECK(a.clock.timeMs == b.clock.timeMs); CHECK(a.clock.steps == b.clock.steps);
    CHECK(a.clock.sampleTimeMs == b.clock.sampleTimeMs);
    CHECK(a.clock.timeLimitMs == b.clock.timeLimitMs); CHECK(a.clock.stepLimit == b.clock.stepLimit);
    CHECK(a.stop.deadlock == b.stop.deadlock); CHECK(a.stop.timeLimitReached == b.stop.timeLimitReached);
    CHECK(a.stop.stepLimitReached == b.stop.stepLimitReached);
    REQUIRE(a.net.marking.size() == b.net.marking.size());
    for (const auto& [id, state] : a.net.marking)
    {
        CHECK(state.tokens == b.net.marking.at(id).tokens);
        CHECK(state.producedTokens == b.net.marking.at(id).producedTokens);
    }
    REQUIRE(a.net.transitionStates.size() == b.net.transitionStates.size());
    for (const auto& [id, state] : a.net.transitionStates)
    {
        const auto& other = b.net.transitionStates.at(id);
        CHECK(state.remainingDelayMs == other.remainingDelayMs); CHECK(state.consumedTokens == other.consumedTokens);
        CHECK(state.active == other.active); CHECK(state.enabled == other.enabled);
    }
}
void Step(SimulationEngine& engine, const StepRandomInputs& random = {})
{
    REQUIRE(std::holds_alternative<SimulationStepPlan>(engine.Step(random)));
}
}

TEST_CASE("Engine creation preserves supplied state and does not require a random step", "[engine]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::Uniform;
    net.snapshot.net.transitions.front().initialDelayMs = 2;
    net.snapshot.net.transitions.front().rangeMaxDelayMs = 7;
    const auto engine = Engine(net, StepClock{7, 4, 3, 100, 50});
    Same(engine.Snapshot(), {net.snapshot, StepClock{7, 4, 3, 100, 50}, {}});
}
TEST_CASE("Engine creation rejects invalid topology range and settings", "[engine]")
{
    Net net;
    StepClock clock;
    auto code = ValidationCode::MissingArcReference;
    SECTION("Topology") { net.topology.at(input).outgoing.clear(); }
    SECTION("State") { net.snapshot.marking.clear(); code = ValidationCode::MissingState; }
    SECTION("Eligibility range") { net.snapshot.transitionStates.at(transition).remainingDelayMs = 2147483648LL; code = ValidationCode::UnsupportedLegacyRange; }
    SECTION("Timing range") { net.snapshot.net.transitions.front().initialDelayMs = 2147483648LL; code = ValidationCode::UnsupportedLegacyRange; }
    SECTION("Interval") { clock.sampleTimeMs = 0; code = ValidationCode::InvalidSimulationSettings; }
    SECTION("Time") { clock.timeMs = -1; code = ValidationCode::InvalidSimulationSettings; }
    SECTION("Steps") { clock.steps = -1; code = ValidationCode::InvalidSimulationSettings; }
    SECTION("Time limit") { clock.timeLimitMs = -1; code = ValidationCode::InvalidSimulationSettings; }
    SECTION("Step limit") { clock.stepLimit = -1; code = ValidationCode::InvalidSimulationSettings; }
    const auto result = SimulationEngine::Create(net.snapshot, net.topology, clock);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    const auto& issues = std::get<std::vector<ValidationIssue>>(result);
    CHECK(std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == code; }));
}
TEST_CASE("Engine owns input topology and returns independent snapshots", "[engine]")
{
    Net net;
    auto engine = Engine(net);
    auto detached = engine.Snapshot();
    detached.net.marking.at(input).tokens = 0;
    detached.clock.steps = 99;
    net.snapshot.net.places.front().name = u"Changed";
    net.snapshot.marking.at(input).tokens = 0;
    net.topology.clear();
    Step(engine);
    CHECK(engine.Snapshot().net.marking.at(input).tokens == 1);
    CHECK(engine.Snapshot().net.net.places.front().name == u"Input");
    CHECK(engine.Snapshot().clock.steps == 1);
}
TEST_CASE("Engine commits successive steps and records deadlock", "[engine]")
{
    auto engine = Engine(Net{});
    Step(engine); Step(engine); Step(engine);
    const auto state = engine.Snapshot();
    CHECK(state.net.marking.at(input).tokens == 0);
    CHECK(state.net.marking.at(output).tokens == 2);
    CHECK(state.net.transitionStates.at(transition).consumedTokens == 11);
    CHECK(state.clock.steps == 3);
    CHECK(state.stop.deadlock);
}
TEST_CASE("Late engine step failure preserves every current state field and supports retry", "[engine]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::Uniform;
    net.snapshot.net.transitions.front().initialDelayMs = 2;
    net.snapshot.net.transitions.front().rangeMaxDelayMs = 7;
    auto engine = Engine(net);
    const auto before = engine.Snapshot();
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(engine.Step()));
    Same(engine.Snapshot(), before);
    Step(engine, {{}, {{transition, 2}}});
    const auto committed = engine.Snapshot();
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(engine.Step({{0}, {}})));
    Same(engine.Snapshot(), committed);
    Step(engine);
    CHECK(engine.Snapshot().net.transitionStates.at(transition).remainingDelayMs == 1);
}
TEST_CASE("Failed engine step retains previous simultaneous stop flags", "[engine]")
{
    Net net;
    net.snapshot.marking.at(input).tokens = 0;
    auto engine = Engine(net, StepClock{2, 0, 1, 1, 0});
    Step(engine);
    const auto stopped = engine.Snapshot();
    CHECK(stopped.stop.deadlock); CHECK(stopped.stop.timeLimitReached); CHECK(stopped.stop.stepLimitReached);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(engine.Step({{0}, {}})));
    Same(engine.Snapshot(), stopped);
}
TEST_CASE("Mutating returned step plans cannot change the engine", "[engine]")
{
    auto engine = Engine(Net{});
    auto result = engine.Step();
    REQUIRE(std::holds_alternative<SimulationStepPlan>(result));
    auto& plan = std::get<SimulationStepPlan>(result);
    plan.snapshotAfter.marking.at(input).tokens = 0;
    plan.clockAfter.steps = 80;
    CHECK(engine.Snapshot().net.marking.at(input).tokens == 1);
    CHECK(engine.Snapshot().clock.steps == 1);
}
TEST_CASE("Legacy engine reset retains timed countdown but clears marking statistics and counters", "[engine]")
{
    for (auto mode : {TimeMode::FixedDelay, TimeMode::Uniform, TimeMode::Exponential})
    {
        Net net;
        net.snapshot.net.transitions.front().mode = mode;
        net.snapshot.net.transitions.front().initialDelayMs = 2;
        net.snapshot.net.transitions.front().rangeMaxDelayMs = 7;
        net.snapshot.transitionStates.at(transition).remainingDelayMs = 2;
        auto engine = Engine(net, StepClock{5, 3, 1, 100, 50});
        Step(engine);
        engine.ResetLegacy();
        const auto state = engine.Snapshot();
        CHECK(state.net.marking.at(input).tokens == 5);
        CHECK(state.net.marking.at(input).producedTokens == 0);
        CHECK(state.net.marking.at(output).producedTokens == 0);
        CHECK(state.net.transitionStates.at(transition).remainingDelayMs == 1);
        CHECK(state.net.transitionStates.at(transition).consumedTokens == 0);
        CHECK_FALSE(state.net.transitionStates.at(transition).active);
        CHECK_FALSE(state.net.transitionStates.at(transition).enabled);
        CHECK(state.clock.timeMs == 0); CHECK(state.clock.steps == 0);
        CHECK(state.clock.sampleTimeMs == 1); CHECK(state.clock.timeLimitMs == 100); CHECK(state.clock.stepLimit == 50);
        CHECK_FALSE(state.stop.Locked());
        engine.ResetLegacy();
        Same(engine.Snapshot(), state);
    }
}
TEST_CASE("Legacy engine reset restores immediate delay to configured start", "[engine]")
{
    Net net;
    net.snapshot.net.transitions.front().initialDelayMs = 3;
    net.snapshot.transitionStates.at(transition).remainingDelayMs = -1;
    auto engine = Engine(net);
    Step(engine);
    engine.ResetLegacy();
    CHECK(engine.Snapshot().net.transitionStates.at(transition).remainingDelayMs == 3);
    CHECK(engine.Snapshot().net.marking.at(input).tokens == 5);
}
TEST_CASE("Initial snapshot restoration is exact and distinct from document reset", "[engine]")
{
    const Net net;
    auto engine = Engine(net, StepClock{7, 4, 2, 6, 3});
    const auto initial = engine.Snapshot();
    CHECK(initial.stop.timeLimitReached); CHECK(initial.stop.stepLimitReached);
    Step(engine);
    engine.ResetLegacy();
    CHECK(engine.Snapshot().net.marking.at(input).tokens == 5);
    engine.RestoreInitialSnapshot();
    Same(engine.Snapshot(), initial);
    CHECK(engine.Snapshot().net.marking.at(input).tokens == 2);
    CHECK(engine.Snapshot().net.transitionStates.at(transition).consumedTokens == 9);
}
TEST_CASE("Independent engines replay identical explicit random inputs", "[engine]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::Uniform;
    net.snapshot.net.transitions.front().initialDelayMs = 2;
    net.snapshot.net.transitions.front().rangeMaxDelayMs = 7;
    auto first = Engine(net);
    auto second = Engine(net);
    for (const auto& random : std::vector<StepRandomInputs>{{{}, {{transition, 2}}}, {}, {}, {{}, {{transition, 4}}}})
    {
        Step(second, random); Step(first, random);
        Same(first.Snapshot(), second.Snapshot());
    }
}
TEST_CASE("Engine steps remain explicit after stop flags and empty reset clears them", "[engine]")
{
    auto created = SimulationEngine::Create({}, {}, StepClock{0, 0, 1, 0, 0});
    REQUIRE(std::holds_alternative<SimulationEngine>(created));
    auto engine = std::get<SimulationEngine>(std::move(created));
    Step(engine); Step(engine);
    CHECK(engine.Snapshot().clock.steps == 2);
    CHECK(engine.Snapshot().stop.deadlock); CHECK(engine.Snapshot().stop.stepLimitReached);
    engine.ResetLegacy();
    CHECK_FALSE(engine.Snapshot().stop.Locked());
    CHECK(engine.Snapshot().clock.steps == 0);
}
