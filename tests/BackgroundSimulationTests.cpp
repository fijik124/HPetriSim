#include "simulation/BackgroundSimulation.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Background service headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <latch>
#include <stdexcept>

using namespace hps::domain;
using namespace hps::simulation;
using namespace std::chrono_literals;
namespace
{
constexpr PlaceId place{1};
constexpr TransitionId transition{2};
struct Net
{
    NetSnapshot snapshot;
    ConflictTopology topology;
    Net()
    {
        snapshot.net.places = {{place, u"P", 10, 8}};
        snapshot.marking = {{place, {8, 0}}};
        snapshot.net.transitions = {{transition, u"T", TimeMode::Uniform, 2, 7}};
        snapshot.transitionStates = {{transition, {}}};
        snapshot.net.arcs = {{ArcId{3}, u"In", place, transition}, {ArcId{4}, u"Out", transition, place}};
        topology = {{place, {{ArcId{4}}, {ArcId{3}}}}, {transition, {{ArcId{3}}, {ArcId{4}}}}};
    }
    SimulationEngine Engine(std::int64_t limit = 10000000) const
    {
        auto created = SimulationEngine::Create(snapshot, topology, StepClock{0, 0, 1, limit, limit}, 1);
        REQUIRE(std::holds_alternative<SimulationEngine>(created));
        return std::get<SimulationEngine>(std::move(created));
    }
};
CommandReply Command(BackgroundSimulation& service, ServiceCommand command)
{
    auto reply = service.Submit(command);
    REQUIRE(reply.wait_for(5s) == std::future_status::ready);
    return reply.get();
}
template<class Predicate>
std::shared_ptr<const PublishedSimulation> Until(BackgroundSimulation& service, Predicate predicate)
{
    auto snapshot = service.Snapshot();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate(*snapshot) && std::chrono::steady_clock::now() < deadline)
        snapshot = service.WaitForUpdate(snapshot->revision, 100ms);
    REQUIRE(predicate(*snapshot));
    return snapshot;
}
void Same(const EngineSnapshot& a, const EngineSnapshot& b)
{
    CHECK(a.random == b.random);
    CHECK(a.clock.steps == b.clock.steps); CHECK(a.clock.timeMs == b.clock.timeMs);
    CHECK(a.stop.deadlock == b.stop.deadlock); CHECK(a.stop.timeLimitReached == b.stop.timeLimitReached);
    CHECK(a.stop.stepLimitReached == b.stop.stepLimitReached);
    for (const auto& [id, value] : a.net.marking)
    {
        CHECK(value.tokens == b.net.marking.at(id).tokens);
        CHECK(value.producedTokens == b.net.marking.at(id).producedTokens);
    }
    for (const auto& [id, value] : a.net.transitionStates)
    {
        const auto& actual = b.net.transitionStates.at(id);
        CHECK(value.remainingDelayMs == actual.remainingDelayMs); CHECK(value.consumedTokens == actual.consumedTokens);
        CHECK(value.active == actual.active); CHECK(value.enabled == actual.enabled);
    }
}
}

TEST_CASE("Background service starts idle and timeout does not invent a publication", "[background]")
{
    BackgroundSimulation service(Net{}.Engine());
    const auto initial = service.Snapshot();
    CHECK(initial->state == RunState::Paused); CHECK_FALSE(initial->closed);
    const auto unchanged = service.WaitForUpdate(initial->revision, 10ms);
    CHECK(unchanged->revision == initial->revision); CHECK(unchanged->engine.clock.steps == 0);
    service.Shutdown();
    CHECK(service.Snapshot()->closed);
}
TEST_CASE("Background batches match the synchronous seeded trajectory at simulation limit", "[background]")
{
    const Net net;
    BackgroundSimulation service(net.Engine(25), 3);
    CHECK(Command(service, ServiceCommand::Resume).outcome == CommandOutcome::Applied);
    const auto stopped = Until(service, [](const auto& state) { return state.engine.stop.Locked(); });
    CHECK(stopped->state == RunState::Paused);
    CHECK(stopped->lastEnd == RunEndReason::SimulationStopped);
    RunController reference(net.Engine(25));
    REQUIRE(reference.Resume() == ResumeResult::Running);
    const auto expected = reference.RunBatch(100);
    Same(stopped->engine, expected.snapshot);
    CHECK(Command(service, ServiceCommand::Resume).outcome == CommandOutcome::SimulationStopped);
}
TEST_CASE("Pause acknowledges a stable state and resume makes further progress", "[background]")
{
    BackgroundSimulation service(Net{}.Engine(), 8);
    Command(service, ServiceCommand::Resume);
    Until(service, [](const auto& state) { return state.engine.clock.steps > 0; });
    const auto paused = Command(service, ServiceCommand::Pause);
    CHECK(paused.outcome == CommandOutcome::Applied); CHECK(paused.snapshot->state == RunState::Paused);
    Same(service.Snapshot()->engine, paused.snapshot->engine);
    const auto revision = paused.snapshot->revision;
    CHECK(service.WaitForUpdate(revision, 10ms)->revision == revision);
    Command(service, ServiceCommand::Resume);
    Until(service, [&](const auto& state) { return state.engine.clock.steps > paused.snapshot->engine.clock.steps; });
    CHECK(Command(service, ServiceCommand::Pause).snapshot->state == RunState::Paused);
}
TEST_CASE("Stop is acknowledged before reset and cannot be resumed implicitly", "[background]")
{
    BackgroundSimulation service(Net{}.Engine(), 8);
    Command(service, ServiceCommand::Resume);
    const auto stopped = Command(service, ServiceCommand::Stop);
    CHECK(stopped.snapshot->state == RunState::Stopped);
    CHECK(Command(service, ServiceCommand::Resume).outcome == CommandOutcome::ResetRequired);
    Same(service.Snapshot()->engine, stopped.snapshot->engine);
    const auto reset = Command(service, ServiceCommand::ResetLegacy);
    CHECK(reset.snapshot->state == RunState::Paused); CHECK(reset.snapshot->engine.clock.steps == 0);
    CHECK(Command(service, ServiceCommand::Resume).outcome == CommandOutcome::Applied);
    Command(service, ServiceCommand::Pause);
}
TEST_CASE("Restore during a run returns the initial state and old tokens cannot cancel a new run", "[background]")
{
    BackgroundSimulation service(Net{}.Engine(), 16);
    const auto initial = service.Snapshot();
    Command(service, ServiceCommand::Resume);
    Until(service, [](const auto& state) { return state.engine.clock.steps > 0; });
    const auto restored = Command(service, ServiceCommand::RestoreInitialSnapshot);
    CHECK(restored.snapshot->state == RunState::Paused);
    Same(restored.snapshot->engine, initial->engine);
    Command(service, ServiceCommand::Resume);
    Until(service, [](const auto& state) { return state.engine.clock.steps > 0; });
    CHECK(Command(service, ServiceCommand::Pause).snapshot->state == RunState::Paused);
}
TEST_CASE("Domain failure is published and remains visible until explicit reset", "[background]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::Immediate;
    net.snapshot.transitionStates.at(transition).consumedTokens = std::numeric_limits<std::int32_t>::max() - 1;
    BackgroundSimulation service(net.Engine());
    Command(service, ServiceCommand::Resume);
    const auto failed = Until(service, [](const auto& state) { return state.state == RunState::Failed; });
    REQUIRE_FALSE(failed->issues.empty());
    CHECK(failed->issues.front().code == ValidationCode::ArithmeticOverflow);
    CHECK(failed->engine.clock.steps == 1);
    CHECK(Command(service, ServiceCommand::Resume).outcome == CommandOutcome::ResetRequired);
    const auto stopped = Command(service, ServiceCommand::Stop);
    CHECK(stopped.snapshot->state == RunState::Failed); CHECK_FALSE(stopped.snapshot->issues.empty());
    const auto reset = Command(service, ServiceCommand::ResetLegacy);
    CHECK(reset.snapshot->issues.empty()); CHECK(reset.snapshot->state == RunState::Paused);
}
TEST_CASE("Generated delay failure publishes unchanged random and model state", "[background]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::Exponential;
    net.snapshot.net.transitions.front().initialDelayMs = std::numeric_limits<std::int32_t>::max();
    auto created = SimulationEngine::Create(net.snapshot, net.topology, {}, 10000);
    REQUIRE(std::holds_alternative<SimulationEngine>(created));
    BackgroundSimulation service(std::get<SimulationEngine>(std::move(created)));
    const auto before = service.Snapshot();
    Command(service, ServiceCommand::Resume);
    const auto failed = Until(service, [](const auto& state) { return state.state == RunState::Failed; });
    Same(failed->engine, before->engine);
    REQUIRE_FALSE(failed->issues.empty());
}
TEST_CASE("Snapshot handles remain independent and valid after service destruction", "[background]")
{
    std::shared_ptr<const PublishedSimulation> retained;
    {
        BackgroundSimulation service(Net{}.Engine());
        retained = service.Snapshot();
        Command(service, ServiceCommand::Resume);
        Until(service, [](const auto& state) { return state.engine.clock.steps > 0; });
    }
    CHECK(retained->engine.clock.steps == 0);
    CHECK(retained->engine.net.marking.at(place).tokens == 8);
    CHECK(retained->engine.random.value == 1);
}
TEST_CASE("Destructor cooperatively joins both idle and running workers", "[background]")
{
    for (bool running : {false, true})
    {
        std::shared_future<void> completed;
        {
            BackgroundSimulation service(Net{}.Engine(), 4);
            completed = service.Completion();
            if (running)
            {
                Command(service, ServiceCommand::Resume);
                Until(service, [](const auto& state) { return state.engine.clock.steps > 0; });
            }
        }
        CHECK(completed.wait_for(0ms) == std::future_status::ready);
        completed.get();
    }
}
TEST_CASE("Shutdown wakes update waiters and rejects further commands explicitly", "[background]")
{
    BackgroundSimulation service(Net{}.Engine());
    const auto revision = service.Snapshot()->revision;
    auto waiter = std::async(std::launch::async, [&] { return service.WaitForUpdate(revision, 5s); });
    service.Shutdown();
    REQUIRE(waiter.wait_for(5s) == std::future_status::ready);
    CHECK(waiter.get()->closed);
    CHECK(Command(service, ServiceCommand::Resume).outcome == CommandOutcome::Closed);
    service.Shutdown();
    CHECK(service.Completion().wait_for(0ms) == std::future_status::ready);
}
TEST_CASE("Shutdown settles queued command futures and supports concurrent join callers", "[background]")
{
    BackgroundSimulation service(Net{}.Engine());
    std::vector<std::future<CommandReply>> pending;
    for (int index = 0; index < 20; ++index) pending.push_back(service.Submit(ServiceCommand::Resume));
    auto firstJoin = std::async(std::launch::async, [&] { service.Shutdown(); });
    auto secondJoin = std::async(std::launch::async, [&] { service.Shutdown(); });
    firstJoin.get(); secondJoin.get();
    for (auto& reply : pending)
    {
        REQUIRE(reply.wait_for(0ms) == std::future_status::ready);
        const auto outcome = reply.get().outcome;
        CHECK((outcome == CommandOutcome::Applied || outcome == CommandOutcome::Closed));
    }
    CHECK(service.Snapshot()->closed);
}
TEST_CASE("Concurrent snapshot readers observe complete coherent publications", "[background]")
{
    BackgroundSimulation service(Net{}.Engine(50), 1);
    std::latch ready(2);
    auto read = [&] {
        auto current = service.Snapshot();
        std::uint64_t revision = current->revision;
        std::int64_t steps = current->engine.clock.steps;
        bool coherent = true;
        ready.count_down();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!current->engine.stop.Locked() && std::chrono::steady_clock::now() < deadline)
        {
            const auto state = service.WaitForUpdate(revision, 100ms);
            coherent = coherent && state->revision >= revision && state->engine.clock.steps >= steps;
            coherent = coherent && state->engine.net.marking.at(place).tokens == 8;
            coherent = coherent && state->engine.net.marking.at(place).producedTokens ==
                state->engine.net.transitionStates.at(transition).consumedTokens;
            revision = state->revision; steps = state->engine.clock.steps;
            current = state;
        }
        return coherent && current->engine.stop.Locked();
    };
    auto a = std::async(std::launch::async, read);
    auto b = std::async(std::launch::async, read);
    ready.wait();
    Command(service, ServiceCommand::Resume);
    Until(service, [](const auto& state) { return state.engine.stop.Locked(); });
    CHECK(a.get()); CHECK(b.get());
}
TEST_CASE("Multiple command producers serialize without accessing the controller directly", "[background]")
{
    BackgroundSimulation service(Net{}.Engine(), 2);
    auto submit = [&] {
        std::vector<std::future<CommandReply>> replies;
        for (int index = 0; index < 10; ++index)
            replies.push_back(service.Submit(index % 2 == 0 ? ServiceCommand::Resume : ServiceCommand::Pause));
        return replies;
    };
    auto a = std::async(std::launch::async, submit);
    auto b = std::async(std::launch::async, submit);
    for (auto* group : {&a, &b})
        for (auto& reply : group->get())
        {
            REQUIRE(reply.wait_for(5s) == std::future_status::ready);
            CHECK(reply.get().outcome == CommandOutcome::Applied);
        }
    CHECK(Command(service, ServiceCommand::Pause).snapshot->state == RunState::Paused);
}
TEST_CASE("Invalid service configuration and commands report explicit errors", "[background]")
{
    const Net net;
    CHECK_THROWS_AS(BackgroundSimulation(net.Engine(), 0), std::invalid_argument);
    CHECK_THROWS_AS(BackgroundSimulation(net.Engine(), 1, 0), std::invalid_argument);
    BackgroundSimulation service(net.Engine());
    CHECK_THROWS_AS(service.Submit(static_cast<ServiceCommand>(99)), std::invalid_argument);
    CHECK(service.Snapshot()->engine.clock.steps == 0);
}
