#include "domain/Timing.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Timing headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>

using namespace hps::domain;
namespace
{
constexpr TransitionId transition{3};
NetSnapshot Net()
{
    NetSnapshot net;
    net.net.places = {{PlaceId{1}, u"Input", 10, 0}, {PlaceId{2}, u"Output", 10, 0}};
    net.net.transitions = {{transition, u"T", TimeMode::FixedDelay, 2, 7}};
    net.net.arcs = {{ArcId{4}, u"In", PlaceId{1}, transition, ArcKind::Normal, 2},
                    {ArcId{5}, u"Out", transition, PlaceId{2}, ArcKind::Normal, 1}};
    net.marking = {{PlaceId{1}, {4, 0}}, {PlaceId{2}, {0, 0}}};
    net.transitionStates = {{transition, {0, 0, false, false}}};
    return net;
}
TimingPlan Plan(const NetSnapshot& net, const std::vector<TransitionId>& selected = {},
    const StepClock& clock = {}, const SampledDelays& samples = {})
{
    const auto result = PlanLegacyTiming(net, selected, clock, samples);
    REQUIRE(std::holds_alternative<TimingPlan>(result));
    return std::get<TimingPlan>(result);
}
bool HasError(const NetSnapshot& net, const std::vector<TransitionId>& selected,
    const StepClock& clock, const SampledDelays& samples, ValidationCode code)
{
    const auto result = PlanLegacyTiming(net, selected, clock, samples);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    const auto& issues = std::get<std::vector<ValidationIssue>>(result);
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == code; });
}
}

TEST_CASE("Waiting timers count down while input remains unchanged", "[timing]")
{
    auto net = Net();
    net.transitionStates.at(transition).remainingDelayMs = 2;
    const auto plan = Plan(net);
    CHECK(plan.transitionsAfter.at(transition).remainingDelayMs == 1);
    CHECK(plan.transitionsAfter.at(transition).enabled);
    CHECK_FALSE(plan.transitionsAfter.at(transition).active);
    CHECK(plan.clockAfter.timeMs == 1);
    CHECK(plan.clockAfter.steps == 1);
    CHECK_FALSE(plan.stop.Locked());
    CHECK(net.transitionStates.at(transition).remainingDelayMs == 2);
}
TEST_CASE("Countdown overshoot remains signed and fires on the next step", "[timing]")
{
    auto net = Net();
    net.transitionStates.at(transition).remainingDelayMs = 1;
    StepClock clock;
    clock.sampleTimeMs = 3;
    const auto waiting = Plan(net, {}, clock);
    CHECK(waiting.transitionsAfter.at(transition).remainingDelayMs == -2);
    CHECK(waiting.transitionsAfter.at(transition).enabled);
    net.transitionStates.at(transition).remainingDelayMs = -2;
    net.transitionStates.at(transition).enabled = true;
    const auto firing = Plan(net, {transition}, waiting.clockAfter);
    CHECK(firing.transitionsAfter.at(transition).remainingDelayMs == 2);
    CHECK(firing.transitionsAfter.at(transition).active);
    CHECK(firing.clockAfter.timeMs == 3);
}
TEST_CASE("Fixed delay restart depends on previous Enabled state", "[timing]")
{
    auto net = Net();
    net.marking.at(PlaceId{1}).tokens = 0;
    net.transitionStates.at(transition).remainingDelayMs = 1;
    SECTION("Previously enabled and now blocked")
    {
        net.transitionStates.at(transition).enabled = true;
        CHECK(Plan(net).transitionsAfter.at(transition).remainingDelayMs == 2);
    }
    SECTION("Previously disabled and still blocked")
    {
        CHECK(Plan(net).transitionsAfter.at(transition).remainingDelayMs == 1);
    }
    SECTION("Previously enabled candidate deselected by conflict")
    {
        net.marking.at(PlaceId{1}).tokens = 4;
        net.transitionStates.at(transition).remainingDelayMs = 0;
        net.transitionStates.at(transition).enabled = true;
        const auto plan = Plan(net);
        CHECK(plan.transitionsAfter.at(transition).remainingDelayMs == 2);
        CHECK_FALSE(plan.transitionsAfter.at(transition).active);
        CHECK(plan.stop.deadlock);
    }
}
TEST_CASE("Disabled stochastic timers retain their remaining delay", "[timing]")
{
    for (auto mode : {TimeMode::Exponential, TimeMode::Uniform})
    {
        auto net = Net();
        net.net.transitions[0].mode = mode;
        net.marking.at(PlaceId{1}).tokens = 0;
        net.transitionStates.at(transition) = {1, 0, false, true};
        const auto plan = Plan(net);
        CHECK(plan.transitionsAfter.at(transition).remainingDelayMs == 1);
        CHECK_FALSE(plan.transitionsAfter.at(transition).enabled);
    }
}
TEST_CASE("Firing restarts fixed delay but retains immediate signed delay", "[timing]")
{
    auto net = Net();
    CHECK(Plan(net, {transition}).transitionsAfter.at(transition).remainingDelayMs == 2);
    net.net.transitions[0].mode = TimeMode::Immediate;
    net.transitionStates.at(transition).remainingDelayMs = -1;
    const auto plan = Plan(net, {transition});
    CHECK(plan.transitionsAfter.at(transition).remainingDelayMs == -1);
    CHECK(plan.transitionsAfter.at(transition).active);
    CHECK_FALSE(plan.transitionsAfter.at(transition).enabled);
}
TEST_CASE("Normal token transfers freeze the clock and other waiting timers", "[timing]")
{
    auto net = Net();
    net.net.transitions.push_back({TransitionId{6}, u"Waiting", TimeMode::FixedDelay, 3, 0});
    net.transitionStates.emplace(TransitionId{6}, TransitionState{3, 0, false, false});
    const auto plan = Plan(net, {transition});
    CHECK(plan.transitionsAfter.at(TransitionId{6}).remainingDelayMs == 3);
    CHECK(plan.transitionsAfter.at(TransitionId{6}).enabled);
    CHECK(plan.clockAfter.timeMs == 0);
}
TEST_CASE("Read-only firings permit other timers and time to advance", "[timing]")
{
    auto net = Net();
    for (auto& arc : net.net.arcs) arc.kind = ArcKind::Test;
    net.net.transitions.push_back({TransitionId{6}, u"Waiting", TimeMode::FixedDelay, 3, 0});
    net.transitionStates.emplace(TransitionId{6}, TransitionState{3, 0, false, false});
    const auto plan = Plan(net, {transition});
    CHECK(plan.transitionsAfter.at(TransitionId{6}).remainingDelayMs == 2);
    CHECK(plan.clockAfter.timeMs == 1);
}
TEST_CASE("Immediate mode with positive delay stays enabled but deadlocks", "[timing]")
{
    auto net = Net();
    net.net.transitions[0].mode = TimeMode::Immediate;
    net.transitionStates.at(transition).remainingDelayMs = 1;
    const auto plan = Plan(net);
    CHECK(plan.transitionsAfter.at(transition).remainingDelayMs == 1);
    CHECK(plan.transitionsAfter.at(transition).enabled);
    CHECK(plan.stop.deadlock);
    CHECK(plan.clockAfter.timeMs == 0);
}
TEST_CASE("An isolated selected transition advances time without ticking its restarted timer", "[timing]")
{
    auto net = Net();
    net.net.arcs.clear();
    const auto plan = Plan(net, {transition});
    CHECK(plan.clockAfter.timeMs == 1);
    CHECK(plan.transitionsAfter.at(transition).remainingDelayMs == 2);
    CHECK_FALSE(plan.stop.deadlock);
}
TEST_CASE("Uniform resampling is supplied explicitly and is repeatable", "[timing]")
{
    auto net = Net();
    net.net.transitions[0].mode = TimeMode::Uniform;
    const SampledDelays samples = {{transition, 4}};
    const auto first = Plan(net, {transition}, {}, samples);
    const auto second = Plan(net, {transition}, {}, samples);
    CHECK(first.transitionsAfter.at(transition).remainingDelayMs == 4);
    CHECK(second.transitionsAfter.at(transition).remainingDelayMs == 4);
    CHECK(first.clockAfter.timeMs == second.clockAfter.timeMs);
    CHECK(net.transitionStates.at(transition).remainingDelayMs == 0);
}
TEST_CASE("Explicit exponential samples may include a zero delay", "[timing]")
{
    auto net = Net();
    net.net.transitions[0].mode = TimeMode::Exponential;
    net.net.transitions[0].initialDelayMs = 1;
    const auto plan = Plan(net, {transition}, {}, {{transition, 0}});
    CHECK(plan.transitionsAfter.at(transition).remainingDelayMs == 0);
    CHECK(plan.clockAfter.timeMs == 0);
}
TEST_CASE("Missing unexpected and invalid samples are diagnosed", "[timing]")
{
    auto net = Net();
    CHECK(HasError(net, {transition}, {}, {{transition, 3}}, ValidationCode::InvalidDelaySample));
    net.net.transitions[0].mode = TimeMode::Uniform;
    CHECK(HasError(net, {transition}, {}, {}, ValidationCode::InvalidDelaySample));
    CHECK(HasError(net, {}, {}, {{transition, 3}}, ValidationCode::InvalidDelaySample));
    CHECK(HasError(net, {transition}, {}, {{transition, 3}, {TransitionId{99}, 3}}, ValidationCode::InvalidDelaySample));
    for (auto invalid : {-1LL, 1LL, 7LL, 2147483648LL})
        CHECK(HasError(net, {transition}, {}, {{transition, invalid}}, ValidationCode::InvalidDelaySample));
}
TEST_CASE("Clock settings reject zero samples negative values and wide counters", "[timing]")
{
    const auto net = Net();
    for (auto sample : {0LL, -1LL, 2147483648LL})
    {
        StepClock clock;
        clock.sampleTimeMs = sample;
        CHECK(HasError(net, {}, clock, {}, ValidationCode::InvalidSimulationSettings));
    }
    StepClock clock;
    clock.timeMs = -1;
    CHECK(HasError(net, {}, clock, {}, ValidationCode::InvalidSimulationSettings));
    clock = {};
    clock.steps = -1;
    CHECK(HasError(net, {}, clock, {}, ValidationCode::InvalidSimulationSettings));
    clock = {};
    clock.timeLimitMs = -1;
    CHECK(HasError(net, {}, clock, {}, ValidationCode::InvalidSimulationSettings));
    clock = {};
    clock.stepLimit = -1;
    CHECK(HasError(net, {}, clock, {}, ValidationCode::InvalidSimulationSettings));
}
TEST_CASE("Limits trigger after crossing equality and retain the completed step", "[timing]")
{
    auto net = Net();
    net.transitionStates.at(transition).remainingDelayMs = 2;
    StepClock clock;
    clock.timeLimitMs = 1;
    clock.stepLimit = 1;
    const auto first = Plan(net, {}, clock);
    CHECK_FALSE(first.stop.Locked());
    net.transitionStates.at(transition).remainingDelayMs = 1;
    net.transitionStates.at(transition).enabled = true;
    const auto second = Plan(net, {}, first.clockAfter);
    CHECK(second.clockAfter.timeMs == 2);
    CHECK(second.clockAfter.steps == 2);
    CHECK(second.stop.timeLimitReached);
    CHECK(second.stop.stepLimitReached);
    CHECK_FALSE(second.stop.deadlock);
}
TEST_CASE("Deadlock time and step limits can be reported together", "[timing]")
{
    auto net = Net();
    net.marking.at(PlaceId{1}).tokens = 0;
    const auto plan = Plan(net, {}, StepClock{2, 1, 1, 1, 1});
    CHECK(plan.stop.deadlock);
    CHECK(plan.stop.timeLimitReached);
    CHECK(plan.stop.stepLimitReached);
    CHECK(plan.stop.Locked());
    CHECK(plan.clockAfter.timeMs == 2);
}
TEST_CASE("Clock arithmetic is checked only for increments that occur", "[timing]")
{
    auto net = Net();
    StepClock clock;
    clock.steps = std::numeric_limits<std::int32_t>::max();
    CHECK(HasError(net, {transition}, clock, {}, ValidationCode::ArithmeticOverflow));
    clock = {};
    clock.timeMs = std::numeric_limits<std::int32_t>::max();
    clock.timeLimitMs = clock.timeMs;
    CHECK_FALSE(Plan(net, {transition}, clock).stop.Locked());
    net.transitionStates.at(transition).remainingDelayMs = 1;
    CHECK(HasError(net, {}, clock, {}, ValidationCode::ArithmeticOverflow));
}
TEST_CASE("Empty nets deadlock and invalid selections remain errors", "[timing]")
{
    const auto empty = Plan(NetSnapshot{});
    CHECK(empty.stop.deadlock);
    CHECK(empty.clockAfter.steps == 1);
    CHECK(empty.clockAfter.timeMs == 0);
    CHECK(HasError(Net(), {TransitionId{99}}, {}, {}, ValidationCode::InvalidSelection));
}
TEST_CASE("Timing configuration outside the legacy range is explicit", "[timing]")
{
    auto net = Net();
    net.net.transitions[0].initialDelayMs = 2147483648LL;
    CHECK(HasError(net, {}, {}, {}, ValidationCode::UnsupportedLegacyRange));
    net = Net();
    net.net.transitions[0].mode = TimeMode::Uniform;
    net.net.transitions[0].rangeMaxDelayMs = 2147483648LL;
    CHECK(HasError(net, {transition}, {}, {{transition, 3}}, ValidationCode::UnsupportedLegacyRange));
}
