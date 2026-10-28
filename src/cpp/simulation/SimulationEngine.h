#pragma once
#include "domain/SimulationStep.h"
#include "SeededStep.h"

namespace hps::simulation
{
struct EngineSnapshot
{
    domain::NetSnapshot net;
    domain::StepClock clock;
    domain::StopFlags stop;
    LegacyRandomState random;
};

// Synchronous, single-owner engine. Value snapshots provide isolation, not
// synchronization: concurrent access still requires an external owner/controller.
class SimulationEngine
{
public:
    static std::variant<SimulationEngine, std::vector<domain::ValidationIssue>> Create(
        const domain::NetSnapshot& snapshot, const domain::ConflictTopology& topology,
        const domain::StepClock& clock = {}, std::uint32_t seed = 1);

    EngineSnapshot Snapshot() const;
    domain::SimulationStepResult Step(const domain::StepRandomInputs& random = {});
    SeededStepResult StepSeeded();
    // Match document Reset, including retaining non-immediate remaining delays.
    void ResetLegacy();
    // Exact restoration of the supplied initial state, not document Reset.
    void RestoreInitialSnapshot();

private:
    SimulationEngine(EngineSnapshot initial, domain::ConflictTopology topology);
    EngineSnapshot initial_;
    EngineSnapshot current_;
    domain::ConflictTopology topology_;
};
}
