#pragma once
#include "domain/SimulationStep.h"

namespace hps::simulation
{
// Explicit compatibility stream, version 1: MSVC-style 32-bit recurrence and
// 15-bit outputs. No process-wide CRT state. This is not a security generator.
struct LegacyRandomState
{
    std::uint32_t value = 1;
    auto operator<=>(const LegacyRandomState&) const = default;
};
class LegacyRandom
{
public:
    explicit LegacyRandom(LegacyRandomState state = {}) : state_(state) {}
    std::uint32_t Next15();
    LegacyRandomState State() const { return state_; }
private:
    LegacyRandomState state_;
};
struct SeededStepPlan
{
    domain::SimulationStepPlan step;
    domain::StepRandomInputs inputs;
    LegacyRandomState randomAfter;
};
using SeededStepResult = std::variant<SeededStepPlan, std::vector<domain::ValidationIssue>>;

// Local stream copy: returns new random state only with a successful full step.
SeededStepResult PlanSeededLegacyStep(const domain::NetSnapshot& snapshot,
    const domain::ConflictTopology& topology, const domain::StepClock& clock,
    LegacyRandomState random);
}
