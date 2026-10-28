#include "simulation/SimulationEngine.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Random step headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace hps::domain;
using namespace hps::simulation;
namespace
{
constexpr PlaceId input{1};
constexpr TransitionId first{2};
struct Net
{
    NetSnapshot snapshot;
    ConflictTopology topology;
    Net(TimeMode mode = TimeMode::Uniform)
    {
        snapshot.net.places = {{input, u"P", 10, 8}};
        snapshot.marking = {{input, {8, 0}}};
        AddTransition(first, mode);
        Arc(input, first);
    }
    void AddTransition(TransitionId id, TimeMode mode = TimeMode::Immediate)
    {
        snapshot.net.transitions.push_back({id, u"T", mode, mode == TimeMode::Exponential ? 10 : 2, 7});
        snapshot.transitionStates.emplace(id, TransitionState{});
        topology.emplace(id, NodeAdjacency{});
    }
    void Arc(NodeId from, NodeId to)
    {
        const ArcId id{static_cast<std::uint32_t>(snapshot.net.arcs.size() + 20)};
        snapshot.net.arcs.push_back({id, u"A", from, to});
        topology[from].outgoing.push_back(id); topology[to].incoming.push_back(id);
    }
    SimulationEngine Engine(std::uint32_t seed = 1) const
    {
        auto created = SimulationEngine::Create(snapshot, topology, {}, seed);
        REQUIRE(std::holds_alternative<SimulationEngine>(created));
        return std::get<SimulationEngine>(std::move(created));
    }
};
SeededStepPlan Step(SimulationEngine& engine)
{
    auto result = engine.StepSeeded();
    REQUIRE(std::holds_alternative<SeededStepPlan>(result));
    return std::get<SeededStepPlan>(std::move(result));
}
}

TEST_CASE("Version one random stream has a pinned seed-one sequence", "[seeded]")
{
    LegacyRandom generator;
    for (std::uint32_t expected : {41U, 18467U, 6334U, 26500U}) CHECK(generator.Next15() == expected);
    auto restored = LegacyRandom(generator.State());
    for (int index = 0; index < 50; ++index) CHECK(restored.Next15() == generator.Next15());
}
TEST_CASE("Uniform seeded input sequence matches the characterized trace", "[seeded]")
{
    const Net net;
    LegacyRandomState random;
    for (std::int64_t sample : {2, 4, 2, 6})
    {
        const auto result = PlanSeededLegacyStep(net.snapshot, net.topology, {}, random);
        REQUIRE(std::holds_alternative<SeededStepPlan>(result));
        const auto& plan = std::get<SeededStepPlan>(result);
        CHECK(plan.inputs.delays.at(first) == sample);
        CHECK(plan.inputs.conflictChoices.empty());
        random = plan.randomAfter;
    }
}
TEST_CASE("Exponential seeded input sequence preserves the legacy formula", "[seeded]")
{
    const Net net(TimeMode::Exponential);
    LegacyRandomState random;
    for (std::int64_t sample : {5, 10, 7, 13})
    {
        const auto result = PlanSeededLegacyStep(net.snapshot, net.topology, {}, random);
        REQUIRE(std::holds_alternative<SeededStepPlan>(result));
        const auto& plan = std::get<SeededStepPlan>(result);
        CHECK(plan.inputs.delays.at(first) == sample);
        random = plan.randomAfter;
    }
}
TEST_CASE("Maximum raw draw maps to the last element of half-open uniform range", "[seeded]")
{
    const Net net;
    std::uint32_t seed = 0;
    for (; seed < 100000; ++seed)
        if (LegacyRandom({seed}).Next15() == 32767) break;
    REQUIRE(seed < 100000);
    const auto result = PlanSeededLegacyStep(net.snapshot, net.topology, {}, {seed});
    REQUIRE(std::holds_alternative<SeededStepPlan>(result));
    CHECK(std::get<SeededStepPlan>(result).inputs.delays.at(first) == 6);
}
TEST_CASE("Conflict choices precede stochastic delays and are replayable explicitly", "[seeded]")
{
    Net net;
    const TransitionId second{3}, producerFirst{4}, producerSecond{5};
    const PlaceId output{6};
    net.snapshot.net.places.push_back({output, u"Out", 10, 0});
    net.snapshot.marking.emplace(output, PlaceState{});
    net.AddTransition(second); net.AddTransition(producerFirst); net.AddTransition(producerSecond);
    net.Arc(input, second); net.Arc(producerFirst, output); net.Arc(producerSecond, output);
    auto engine = net.Engine();
    const auto plan = Step(engine);
    CHECK(plan.inputs.conflictChoices == std::vector<std::int64_t>{0, 1});
    CHECK(plan.inputs.delays.at(first) == 2);
    REQUIRE(plan.step.selection.decisions.size() == 2);
    const auto explicitResult = PlanLegacySimulationStep(net.snapshot, net.topology, {}, plan.inputs);
    REQUIRE(std::holds_alternative<SimulationStepPlan>(explicitResult));
    CHECK(std::get<SimulationStepPlan>(explicitResult).selection.selectedInNetOrder == plan.step.selection.selectedInNetOrder);
    CHECK(engine.Snapshot().random == plan.randomAfter);
}
TEST_CASE("Steps without random work leave the stream unchanged", "[seeded]")
{
    Net net;
    net.snapshot.transitionStates.at(first).remainingDelayMs = 2;
    auto engine = net.Engine(0);
    const auto plan = Step(engine);
    CHECK(plan.inputs.delays.empty()); CHECK(plan.inputs.conflictChoices.empty());
    CHECK(engine.Snapshot().random.value == 0);
    auto empty = SimulationEngine::Create({}, {}, {}, 99);
    REQUIRE(std::holds_alternative<SimulationEngine>(empty));
    auto isolated = std::get<SimulationEngine>(std::move(empty));
    Step(isolated);
    CHECK(isolated.Snapshot().random.value == 99);
}
TEST_CASE("Failed generated steps roll back both stream and model", "[seeded]")
{
    Net net(TimeMode::Exponential);
    std::uint32_t seed = 10000;
    SECTION("Delay overflow") { net.snapshot.net.transitions.front().initialDelayMs = std::numeric_limits<std::int32_t>::max(); }
    SECTION("Token statistics overflow") { net.snapshot.transitionStates.at(first).consumedTokens = std::numeric_limits<std::int32_t>::max(); }
    auto engine = net.Engine(seed);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto result = engine.StepSeeded();
        REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
        CHECK(std::get<std::vector<ValidationIssue>>(result).front().code == ValidationCode::ArithmeticOverflow);
        CHECK(engine.Snapshot().random.value == seed);
        CHECK(engine.Snapshot().clock.steps == 0);
        CHECK(engine.Snapshot().net.marking.at(input).tokens == 8);
    }
}
TEST_CASE("Legacy reset retains stream while initial restoration rewinds it", "[seeded]")
{
    auto engine = Net{}.Engine();
    const auto firstPlan = Step(engine);
    engine.ResetLegacy();
    CHECK(engine.Snapshot().random == firstPlan.randomAfter);
    CHECK(engine.Snapshot().net.transitionStates.at(first).remainingDelayMs == 2);
    engine.RestoreInitialSnapshot();
    CHECK(engine.Snapshot().random.value == 1);
    const auto replay = Step(engine);
    CHECK(replay.inputs.delays == firstPlan.inputs.delays);
    CHECK(replay.randomAfter == firstPlan.randomAfter);
}
TEST_CASE("Seeded engines are independent and different seeds have distinct traces", "[seeded]")
{
    const Net net;
    auto a = net.Engine(); auto b = net.Engine(); auto different = net.Engine(10000);
    CHECK(Step(different).inputs.delays.at(first) == 6);
    for (int index = 0; index < 20; ++index)
    {
        const auto aPlan = Step(a);
        Step(different);
        const auto bPlan = Step(b);
        CHECK(aPlan.inputs.conflictChoices == bPlan.inputs.conflictChoices);
        CHECK(aPlan.inputs.delays == bPlan.inputs.delays);
        CHECK(a.Snapshot().random == b.Snapshot().random);
        CHECK(a.Snapshot().net.marking.at(input).tokens == b.Snapshot().net.marking.at(input).tokens);
    }
}
TEST_CASE("Explicit engine stepping leaves its seeded stream untouched", "[seeded]")
{
    auto engine = Net{}.Engine(99);
    const auto result = engine.Step({{}, {{first, 3}}});
    REQUIRE(std::holds_alternative<SimulationStepPlan>(result));
    CHECK(engine.Snapshot().random.value == 99);
}
