#include "SeededStep.h"
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace hps::simulation
{
std::uint32_t LegacyRandom::Next15()
{
    state_.value = state_.value * std::uint32_t{214013} + std::uint32_t{2531011};
    return (state_.value >> 16) & std::uint32_t{32767};
}

SeededStepResult PlanSeededLegacyStep(const domain::NetSnapshot& snapshot,
    const domain::ConflictTopology& topology, const domain::StepClock& clock,
    LegacyRandomState random)
{
    using namespace domain;
    auto scan = FindLegacyPotentialConflicts(snapshot, topology);
    if (auto* issues = std::get_if<std::vector<ValidationIssue>>(&scan)) return std::move(*issues);
    auto settings = ValidateLegacyTimingSettings(snapshot, clock);
    if (!settings.empty()) return settings;
    LegacyRandom generator(random);
    StepRandomInputs inputs;
    ConflictSelectionPlan selection;
    for (;;)
    {
        auto progress = ContinueLegacyConflictSelection(snapshot, topology,
            std::get<std::vector<TransitionId>>(scan), inputs.conflictChoices);
        if (auto* issues = std::get_if<std::vector<ValidationIssue>>(&progress)) return std::move(*issues);
        if (auto* plan = std::get_if<ConflictSelectionPlan>(&progress))
        {
            selection = std::move(*plan);
            break;
        }
        const auto count = std::get<ConflictChoiceRequest>(progress).activeNormalArcCount;
        const double value = (static_cast<double>(generator.Next15()) / 32767.0) * static_cast<double>(count);
        auto choice = static_cast<std::int64_t>(value);
        if (choice == static_cast<std::int64_t>(count)) --choice;
        inputs.conflictChoices.push_back(choice);
    }
    const std::set<TransitionId> selected(selection.selectedInNetOrder.begin(), selection.selectedInNetOrder.end());
    for (const auto& transition : snapshot.net.transitions)
    {
        if (!selected.contains(transition.id) ||
            (transition.mode != TimeMode::Uniform && transition.mode != TimeMode::Exponential)) continue;
        const auto draw = generator.Next15();
        double value;
        if (transition.mode == TimeMode::Uniform)
        {
            value = (static_cast<double>(draw) / 32767.0) *
                static_cast<double>(transition.rangeMaxDelayMs - transition.initialDelayMs) +
                static_cast<double>(transition.initialDelayMs);
        }
        else
        {
            // Preserve HRandom's legacy formula, including its +0.5 offset.
            const double legacyDouble = (static_cast<double>(draw) / 32767.0) * 32767.0;
            value = static_cast<double>(transition.initialDelayMs) *
                (1.0 - std::log((32767.0 - legacyDouble) / 32767.0 + 0.5));
        }
        if (!std::isfinite(value) || value < 0 || value > std::numeric_limits<std::int32_t>::max())
            return std::vector<ValidationIssue>{{ValidationCode::ArithmeticOverflow, transition.id.value,
                "Generated legacy delay exceeds the supported signed 32-bit range"}};
        auto delay = static_cast<std::int64_t>(value);
        if (transition.mode == TimeMode::Uniform && value == static_cast<double>(transition.rangeMaxDelayMs)) --delay;
        inputs.delays.emplace(transition.id, delay);
    }
    auto result = PlanLegacySimulationStep(snapshot, topology, clock, inputs);
    if (auto* issues = std::get_if<std::vector<ValidationIssue>>(&result)) return std::move(*issues);
    return SeededStepPlan{std::get<SimulationStepPlan>(std::move(result)), std::move(inputs), generator.State()};
}
}
