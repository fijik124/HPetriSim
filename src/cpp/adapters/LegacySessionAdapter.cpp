#include "stdafx.h"
#include "PetriSimDoc.h"
#include "LegacyNetAdapter.h"
#include "LegacySessionAdapter.h"
#include <utility>

namespace hps::adapters
{
application::SessionCreationResult CreateLegacyDocumentSession(CPetriSimDoc& document,
    application::DocumentId identity, std::uint32_t seed)
{
    auto exported = ExportLegacySimulationSnapshot(document);
    if (auto* issues = std::get_if<std::vector<domain::ValidationIssue>>(&exported)) return std::move(*issues);
    const auto& model = std::get<LegacySimulationSnapshot>(exported);
    const auto& simulation = document.Sim();
    application::SessionOptions options;
    options.clock = {simulation.SimTime(), simulation.StepCount(), simulation.SampleTime(),
        simulation.TimeLimit(), simulation.StepLimit()};
    options.seed = seed;
    return application::DocumentSimulationSession::Create(identity, model.snapshot, model.topology, options);
}
}
