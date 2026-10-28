#include "simulation/RunController.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Controller headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace hps::domain;
using namespace hps::simulation;
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
    RunController Controller(const StepClock& clock = {}, std::uint32_t seed = 1) const
    {
        auto created = SimulationEngine::Create(snapshot, topology, clock, seed);
        REQUIRE(std::holds_alternative<SimulationEngine>(created));
        return RunController(std::get<SimulationEngine>(std::move(created)));
    }
};
void Same(const EngineSnapshot& a, const EngineSnapshot& b)
{
    CHECK(a.random == b.random);
    CHECK(a.clock.steps == b.clock.steps); CHECK(a.clock.timeMs == b.clock.timeMs);
    CHECK(a.clock.sampleTimeMs == b.clock.sampleTimeMs);
    CHECK(a.clock.timeLimitMs == b.clock.timeLimitMs); CHECK(a.clock.stepLimit == b.clock.stepLimit);
    CHECK(a.stop.deadlock == b.stop.deadlock); CHECK(a.stop.timeLimitReached == b.stop.timeLimitReached);
    CHECK(a.stop.stepLimitReached == b.stop.stepLimitReached);
    for (const auto& [id, state] : a.net.marking)
    {
        CHECK(state.tokens == b.net.marking.at(id).tokens);
        CHECK(state.producedTokens == b.net.marking.at(id).producedTokens);
    }
    for (const auto& [id, state] : a.net.transitionStates)
    {
        const auto& other = b.net.transitionStates.at(id);
        CHECK(state.remainingDelayMs == other.remainingDelayMs); CHECK(state.consumedTokens == other.consumedTokens);
        CHECK(state.active == other.active); CHECK(state.enabled == other.enabled);
    }
}
}

TEST_CASE("Controller starts paused and runs only after explicit resume", "[controller]")
{
    auto controller = Net{}.Controller();
    const auto before = controller.Snapshot();
    CHECK(controller.State() == RunState::Paused);
    const auto paused = controller.RunBatch(10);
    CHECK(paused.reason == RunEndReason::Paused); CHECK(paused.completedSteps == 0);
    Same(paused.snapshot, before);
    CHECK(controller.Resume() == ResumeResult::Running);
    const auto ran = controller.RunBatch(2);
    CHECK(ran.reason == RunEndReason::BudgetExhausted); CHECK(ran.completedSteps == 2);
    CHECK(controller.State() == RunState::Running);
    CHECK(ran.issues.empty());
}
TEST_CASE("Zero budget performs no step and consumes no random value", "[controller]")
{
    auto controller = Net{}.Controller();
    const auto before = controller.Snapshot();
    REQUIRE(controller.Resume() == ResumeResult::Running);
    const auto result = controller.RunBatch(0);
    CHECK(result.reason == RunEndReason::BudgetExhausted); CHECK(result.completedSteps == 0);
    Same(result.snapshot, before);
}
TEST_CASE("Partitioning a run into bounded batches preserves seeded trajectory", "[controller]")
{
    auto whole = Net{}.Controller(); auto chunks = Net{}.Controller();
    REQUIRE(whole.Resume() == ResumeResult::Running); REQUIRE(chunks.Resume() == ResumeResult::Running);
    CHECK(whole.RunBatch(20).completedSteps == 20);
    CHECK(chunks.RunBatch(2).completedSteps == 2);
    CHECK(chunks.RunBatch(3).completedSteps == 3);
    CHECK(chunks.RunBatch(15).completedSteps == 15);
    Same(whole.Snapshot(), chunks.Snapshot());
}
TEST_CASE("Pause and resume preserve model and random progress", "[controller]")
{
    auto controller = Net{}.Controller(); auto reference = Net{}.Controller();
    REQUIRE(controller.Resume() == ResumeResult::Running); REQUIRE(reference.Resume() == ResumeResult::Running);
    controller.RunBatch(3);
    const auto before = controller.Snapshot();
    controller.Pause();
    CHECK(controller.RunBatch(8).reason == RunEndReason::Paused);
    Same(controller.Snapshot(), before);
    REQUIRE(controller.Resume() == ResumeResult::Running);
    controller.RunBatch(5); reference.RunBatch(8);
    Same(controller.Snapshot(), reference.Snapshot());
}
TEST_CASE("Stop retains state and requires an explicit reset before resuming", "[controller]")
{
    auto controller = Net{}.Controller();
    REQUIRE(controller.Resume() == ResumeResult::Running);
    controller.RunBatch(1);
    const auto before = controller.Snapshot();
    controller.Stop();
    CHECK(controller.State() == RunState::Stopped);
    CHECK(controller.Resume() == ResumeResult::ResetRequired);
    const auto result = controller.RunBatch(10);
    CHECK(result.reason == RunEndReason::Stopped); CHECK(result.completedSteps == 0);
    Same(result.snapshot, before);
    controller.ResetLegacy();
    CHECK(controller.State() == RunState::Paused);
    CHECK(controller.Snapshot().random == before.random);
    CHECK(controller.Resume() == ResumeResult::Running);
}
TEST_CASE("Pause request is observed before any new step", "[controller]")
{
    auto controller = Net{}.Controller();
    REQUIRE(controller.Resume() == ResumeResult::Running);
    controller.RunBatch(1);
    const auto before = controller.Snapshot();
    std::stop_source pause;
    pause.request_stop();
    const auto result = controller.RunBatch(10, {pause.get_token(), {}});
    CHECK(result.reason == RunEndReason::Paused); CHECK(result.completedSteps == 0);
    Same(result.snapshot, before);
    REQUIRE(controller.Resume() == ResumeResult::Running);
    CHECK(controller.RunBatch(1).completedSteps == 1); // Tokens belong to each call.
}
TEST_CASE("Stop request takes precedence over pause and zero budget", "[controller]")
{
    auto controller = Net{}.Controller();
    std::stop_source pause, stop;
    pause.request_stop(); stop.request_stop();
    const auto before = controller.Snapshot();
    const auto result = controller.RunBatch(0, {pause.get_token(), stop.get_token()});
    CHECK(result.reason == RunEndReason::Stopped); CHECK(result.completedSteps == 0);
    CHECK(controller.State() == RunState::Stopped);
    Same(result.snapshot, before);
}
TEST_CASE("Step limit pauses immediately even on the final budgeted step", "[controller]")
{
    auto controller = Net{}.Controller(StepClock{0, 0, 1, 1000, 2});
    REQUIRE(controller.Resume() == ResumeResult::Running);
    const auto result = controller.RunBatch(3);
    CHECK(result.completedSteps == 3); CHECK(result.reason == RunEndReason::SimulationStopped);
    CHECK(result.snapshot.stop.stepLimitReached); CHECK(controller.State() == RunState::Paused);
    CHECK(controller.Resume() == ResumeResult::SimulationStopped);
    CHECK(controller.RunBatch(10).completedSteps == 0);
    controller.ResetLegacy();
    CHECK(controller.Resume() == ResumeResult::Running);
}
TEST_CASE("Time limit remains strict across multiple batches", "[controller]")
{
    auto controller = Net{}.Controller(StepClock{0, 0, 1, 1, 1000});
    REQUIRE(controller.Resume() == ResumeResult::Running);
    CHECK(controller.RunBatch(2).reason == RunEndReason::BudgetExhausted);
    CHECK(controller.Snapshot().clock.timeMs == 1);
    const auto result = controller.RunBatch(10);
    CHECK(result.completedSteps == 1); CHECK(result.reason == RunEndReason::SimulationStopped);
    CHECK(result.snapshot.clock.timeMs == 2); CHECK(result.snapshot.stop.timeLimitReached);
}
TEST_CASE("Deadlock is one successful counted step and stops the run", "[controller]")
{
    Net net;
    net.snapshot.marking.at(place).tokens = 0;
    auto controller = net.Controller();
    REQUIRE(controller.Resume() == ResumeResult::Running);
    const auto result = controller.RunBatch(10);
    CHECK(result.completedSteps == 1); CHECK(result.snapshot.clock.steps == 1);
    CHECK(result.reason == RunEndReason::SimulationStopped); CHECK(result.snapshot.stop.deadlock);
}
TEST_CASE("Failure after successful progress retains the last commit and diagnostics", "[controller]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::Immediate;
    net.snapshot.transitionStates.at(transition).consumedTokens = std::numeric_limits<std::int32_t>::max() - 1;
    auto controller = net.Controller();
    REQUIRE(controller.Resume() == ResumeResult::Running);
    const auto result = controller.RunBatch(10);
    CHECK(result.reason == RunEndReason::StepError); CHECK(result.completedSteps == 1);
    CHECK(result.snapshot.clock.steps == 1);
    CHECK(result.snapshot.net.transitionStates.at(transition).consumedTokens == std::numeric_limits<std::int32_t>::max());
    REQUIRE_FALSE(result.issues.empty()); CHECK(result.issues.front().code == ValidationCode::ArithmeticOverflow);
    CHECK(controller.State() == RunState::Failed); CHECK(controller.Resume() == ResumeResult::ResetRequired);
    controller.Pause(); controller.Stop(); // Neither operation hides failure.
    CHECK(controller.State() == RunState::Failed);
    const auto repeated = controller.RunBatch(10);
    CHECK(repeated.reason == RunEndReason::StepError); CHECK(repeated.completedSteps == 0);
    CHECK(repeated.issues.front().code == result.issues.front().code);
    Same(result.snapshot, repeated.snapshot);
    controller.ResetLegacy();
    REQUIRE(controller.Resume() == ResumeResult::Running);
    CHECK(controller.RunBatch(1).issues.empty());
}
TEST_CASE("Generated delay failure preserves the stream and reports no completed step", "[controller]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::Exponential;
    net.snapshot.net.transitions.front().initialDelayMs = std::numeric_limits<std::int32_t>::max();
    auto controller = net.Controller({}, 10000);
    const auto before = controller.Snapshot();
    REQUIRE(controller.Resume() == ResumeResult::Running);
    const auto result = controller.RunBatch(10);
    CHECK(result.reason == RunEndReason::StepError); CHECK(result.completedSteps == 0);
    Same(result.snapshot, before);
}
TEST_CASE("Controller reset and initial restoration keep distinct timing and stream semantics", "[controller]")
{
    auto controller = Net{}.Controller();
    const auto initial = controller.Snapshot();
    REQUIRE(controller.Resume() == ResumeResult::Running);
    controller.RunBatch(1);
    const auto advanced = controller.Snapshot();
    controller.ResetLegacy();
    CHECK(controller.State() == RunState::Paused);
    CHECK(controller.Snapshot().random == advanced.random);
    CHECK(controller.Snapshot().net.transitionStates.at(transition).remainingDelayMs == 2);
    controller.RestoreInitialSnapshot();
    Same(controller.Snapshot(), initial);
}
TEST_CASE("Run reports own their snapshot and initial exceeded limits cannot resume", "[controller]")
{
    auto controller = Net{}.Controller();
    REQUIRE(controller.Resume() == ResumeResult::Running);
    auto result = controller.RunBatch(1);
    result.snapshot.net.marking.at(place).tokens = 0;
    result.snapshot.random.value = 0;
    CHECK(controller.Snapshot().net.marking.at(place).tokens == 8);
    CHECK(controller.Snapshot().random.value != 0);
    auto limited = Net{}.Controller(StepClock{2, 0, 1, 1, 1000});
    CHECK(limited.Resume() == ResumeResult::SimulationStopped);
    CHECK(limited.RunBatch(10).completedSteps == 0);
}
