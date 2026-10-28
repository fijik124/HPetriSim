#pragma once
#include "ConflictScan.h"
#include "ConflictSelection.h"
#include "Timing.h"

namespace hps::domain
{
struct StepRandomInputs
{
    std::vector<std::int64_t> conflictChoices;
    SampledDelays delays;
};
struct SimulationStepPlan
{
    NetSnapshot snapshotAfter;
    StepClock clockAfter;
    StopFlags stop;
    std::vector<TransitionId> potentialConflicts;
    ConflictSelectionPlan selection;
    bool movedTokens = false;
};
using SimulationStepResult = std::variant<SimulationStepPlan, std::vector<ValidationIssue>>;

// One synchronous legacy-compatible step from a stable complete topology.
// Random inputs are final conflict choices and resampled delays, not raw draws.
// Returns an independent complete state or errors; never modifies inputs.
// Stop flags are reported after the step; the caller decides whether to step again.
SimulationStepResult PlanLegacySimulationStep(const NetSnapshot& snapshot,
    const ConflictTopology& topology, const StepClock& clock,
    const StepRandomInputs& random = {});
}
