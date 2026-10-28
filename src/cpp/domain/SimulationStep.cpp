#include "SimulationStep.h"
#include <utility>

namespace hps::domain
{
SimulationStepResult PlanLegacySimulationStep(const NetSnapshot& snapshot,
    const ConflictTopology& topology, const StepClock& clock, const StepRandomInputs& random)
{
    auto scanned = FindLegacyPotentialConflicts(snapshot, topology);
    if (const auto* issues = std::get_if<std::vector<ValidationIssue>>(&scanned)) return *issues;
    const auto& order = std::get<std::vector<TransitionId>>(scanned);
    auto selected = PlanLegacyConflictSelection(snapshot, topology, order, random.conflictChoices);
    if (const auto* issues = std::get_if<std::vector<ValidationIssue>>(&selected)) return *issues;
    const auto& selection = std::get<ConflictSelectionPlan>(selected);
    auto effects = PlanLegacyTokenEffects(snapshot, selection.selectedInNetOrder);
    if (const auto* issues = std::get_if<std::vector<ValidationIssue>>(&effects)) return *issues;
    const auto timed = PlanLegacyTiming(snapshot, selection.selectedInNetOrder, clock, random.delays);
    if (const auto* issues = std::get_if<std::vector<ValidationIssue>>(&timed)) return *issues;

    // Publish a complete value only after every phase has succeeded.
    auto& tokens = std::get<TokenEffectPlan>(effects);
    const auto& timing = std::get<TimingPlan>(timed);
    SimulationStepPlan plan;
    plan.snapshotAfter = snapshot;
    plan.snapshotAfter.marking = std::move(tokens.markingAfter);
    for (auto& [id, state] : plan.snapshotAfter.transitionStates)
    {
        const auto& transition = timing.transitionsAfter.at(id);
        state = {transition.remainingDelayMs, tokens.consumedTokensAfter.at(id),
            transition.active, transition.enabled};
    }
    plan.clockAfter = timing.clockAfter;
    plan.stop = timing.stop;
    plan.potentialConflicts = std::get<std::vector<TransitionId>>(std::move(scanned));
    plan.selection = std::get<ConflictSelectionPlan>(std::move(selected));
    plan.movedTokens = tokens.movedTokens;
    return plan;
}
}
