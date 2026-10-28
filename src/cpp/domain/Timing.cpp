#include "Timing.h"
#include <limits>
#include <set>

namespace hps::domain
{
TimingResult PlanLegacyTiming(const NetSnapshot& snapshot,
    const std::vector<TransitionId>& selected, const StepClock& clock,
    const SampledDelays& samples)
{
    const auto effect = PlanLegacyTokenEffects(snapshot, selected);
    if (const auto* issues = std::get_if<std::vector<ValidationIssue>>(&effect)) return *issues;
    constexpr std::int64_t maximum = std::numeric_limits<std::int32_t>::max();
    std::vector<ValidationIssue> issues;
    auto clockRange = [&](std::int64_t value, std::int64_t minimum, const char* message) {
        if (value < minimum || value > maximum)
            issues.push_back({ValidationCode::InvalidSimulationSettings, 0, message});
    };
    clockRange(clock.timeMs, 0, "Simulation time must fit the nonnegative signed 32-bit legacy range");
    clockRange(clock.steps, 0, "Step count must fit the nonnegative signed 32-bit legacy range");
    clockRange(clock.sampleTimeMs, 1, "Sample interval must be positive and fit the signed 32-bit legacy range");
    clockRange(clock.timeLimitMs, 0, "Time limit must fit the nonnegative signed 32-bit legacy range");
    clockRange(clock.stepLimit, 0, "Step limit must fit the nonnegative signed 32-bit legacy range");
    std::set<TransitionId> selection(selected.begin(), selected.end());
    std::set<TransitionId> needsSample;
    for (const auto& transition : snapshot.net.transitions)
    {
        if (transition.initialDelayMs > maximum ||
            (transition.mode == TimeMode::Uniform && transition.rangeMaxDelayMs > maximum))
            issues.push_back({ValidationCode::UnsupportedLegacyRange, transition.id.value, "Timing configuration exceeds the signed 32-bit legacy range"});
        if (!selection.contains(transition.id) ||
            (transition.mode != TimeMode::Uniform && transition.mode != TimeMode::Exponential)) continue;
        needsSample.insert(transition.id);
        const auto sample = samples.find(transition.id);
        if (sample == samples.end())
            issues.push_back({ValidationCode::InvalidDelaySample, transition.id.value, "Selected stochastic transition needs an explicit sampled delay"});
        else if (sample->second < 0 || sample->second > maximum)
            issues.push_back({ValidationCode::InvalidDelaySample, transition.id.value, "Sampled delay must fit the nonnegative signed 32-bit legacy range"});
        else if (transition.mode == TimeMode::Uniform &&
            (sample->second < transition.initialDelayMs || sample->second >= transition.rangeMaxDelayMs))
            issues.push_back({ValidationCode::InvalidDelaySample, transition.id.value, "Uniform sample is outside its configured half-open range"});
    }
    for (const auto& [id, value] : samples)
    {
        (void)value;
        if (!needsSample.contains(id))
            issues.push_back({ValidationCode::InvalidDelaySample, id.value, "Unexpected sample for an unselected, unknown or deterministic transition"});
    }
    if (!issues.empty()) return issues;
    if (clock.steps == maximum)
        return std::vector<ValidationIssue>{{ValidationCode::ArithmeticOverflow, 0, "Step count would overflow the signed 32-bit legacy range"}};

    const auto evaluated = EvaluateLegacyEligibility(snapshot);
    // Token planning above has already checked eligibility and joint effects.
    const auto& eligibility = std::get<std::vector<TransitionEligibility>>(evaluated);
    TimingPlan plan;
    plan.clockAfter = clock;
    ++plan.clockAfter.steps;
    plan.stop.deadlock = selection.empty();
    const bool movedTokens = std::get<TokenEffectPlan>(effect).movedTokens;
    for (std::size_t index = 0; index < snapshot.net.transitions.size(); ++index)
    {
        const auto& transition = snapshot.net.transitions[index];
        const auto& previous = snapshot.transitionStates.at(transition.id);
        const bool enabled = std::holds_alternative<DelayPending>(eligibility[index].state);
        const bool active = selection.contains(transition.id);
        auto delay = previous.remainingDelayMs;
        if (previous.enabled && !enabled && transition.mode == TimeMode::FixedDelay)
            delay = transition.initialDelayMs;
        if (active)
        {
            switch (transition.mode)
            {
            case TimeMode::Immediate: break;
            case TimeMode::FixedDelay: delay = transition.initialDelayMs; break;
            case TimeMode::Uniform:
            case TimeMode::Exponential: delay = samples.at(transition.id); break;
            }
        }
        if (!movedTokens && enabled && transition.mode != TimeMode::Immediate)
        {
            delay -= clock.sampleTimeMs;
            plan.stop.deadlock = false;
        }
        plan.transitionsAfter.emplace(transition.id, TransitionTiming{delay, active, enabled});
    }
    if (!movedTokens && !plan.stop.deadlock)
    {
        if (clock.sampleTimeMs > maximum - clock.timeMs)
            return std::vector<ValidationIssue>{{ValidationCode::ArithmeticOverflow, 0, "Simulation time would overflow the signed 32-bit legacy range"}};
        plan.clockAfter.timeMs += clock.sampleTimeMs;
    }
    plan.stop.timeLimitReached = plan.clockAfter.timeMs > clock.timeLimitMs;
    plan.stop.stepLimitReached = plan.clockAfter.steps > clock.stepLimit;
    return plan;
}
}
