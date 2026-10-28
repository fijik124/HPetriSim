#include "SimulationEngine.h"
#include <type_traits>
#include <utility>

namespace hps::simulation
{
static_assert(std::is_nothrow_move_assignable_v<EngineSnapshot>);

SimulationEngine::SimulationEngine(EngineSnapshot initial, domain::ConflictTopology topology)
    : initial_(std::move(initial)), current_(initial_), topology_(std::move(topology))
{
}

std::variant<SimulationEngine, std::vector<domain::ValidationIssue>> SimulationEngine::Create(
    const domain::NetSnapshot& snapshot, const domain::ConflictTopology& topology,
    const domain::StepClock& clock, std::uint32_t seed)
{
    auto issues = domain::ValidateConflictTopology(snapshot, topology);
    if (!issues.empty()) return issues;
    const auto eligibility = domain::EvaluateLegacyEligibility(snapshot);
    if (const auto* errors = std::get_if<std::vector<domain::ValidationIssue>>(&eligibility)) return *errors;
    issues = domain::ValidateLegacyTimingSettings(snapshot, clock);
    if (!issues.empty()) return issues;
    return SimulationEngine({snapshot, clock,
        {false, clock.timeMs > clock.timeLimitMs, clock.steps > clock.stepLimit}, {seed}}, topology);
}

EngineSnapshot SimulationEngine::Snapshot() const { return current_; }

domain::SimulationStepResult SimulationEngine::Step(const domain::StepRandomInputs& random)
{
    auto result = domain::PlanLegacySimulationStep(current_.net, topology_, current_.clock, random);
    if (const auto* plan = std::get_if<domain::SimulationStepPlan>(&result))
    {
        // Copy/allocate first. Assignment cannot throw and commits all fields together.
        EngineSnapshot candidate{plan->snapshotAfter, plan->clockAfter, plan->stop, current_.random};
        current_ = std::move(candidate);
    }
    return result;
}

SeededStepResult SimulationEngine::StepSeeded()
{
    auto result = PlanSeededLegacyStep(current_.net, topology_, current_.clock, current_.random);
    if (const auto* plan = std::get_if<SeededStepPlan>(&result))
    {
        EngineSnapshot candidate{plan->step.snapshotAfter, plan->step.clockAfter, plan->step.stop, plan->randomAfter};
        current_ = std::move(candidate);
    }
    return result;
}

void SimulationEngine::ResetLegacy()
{
    auto candidate = current_;
    for (const auto& place : candidate.net.net.places)
        candidate.net.marking.at(place.id) = {place.initialTokens, 0};
    for (const auto& transition : candidate.net.net.transitions)
    {
        auto& state = candidate.net.transitionStates.at(transition.id);
        if (transition.mode == domain::TimeMode::Immediate)
            state.remainingDelayMs = transition.initialDelayMs;
        state.consumedTokens = 0;
        state.active = false;
        state.enabled = false;
    }
    candidate.clock.timeMs = 0;
    candidate.clock.steps = 0;
    candidate.stop = {};
    current_ = std::move(candidate);
}

void SimulationEngine::RestoreInitialSnapshot()
{
    auto candidate = initial_;
    current_ = std::move(candidate);
}
}
