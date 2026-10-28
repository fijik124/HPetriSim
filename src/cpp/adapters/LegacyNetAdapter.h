#pragma once
#include "domain/PetriNet.h"
#include "domain/ConflictTopology.h"

class CPetriSimDoc;

namespace hps::adapters
{
using ExportResult = std::variant<domain::NetSnapshot, std::vector<domain::ValidationIssue>>;

// Call on the document-owning thread while simulation is offline. This is an
// independent value copy, not synchronization for concurrent legacy mutation.
ExportResult ExportLegacyNet(CPetriSimDoc& document);

struct LegacySimulationSnapshot
{
    domain::NetSnapshot snapshot;
    domain::ConflictTopology topology;
};
using SimulationExportResult = std::variant<LegacySimulationSnapshot, std::vector<domain::ValidationIssue>>;
// Same owning-thread/offline requirement. Copies real adjacency order alongside
// document order and validates both as one value; no cached pointers escape.
SimulationExportResult ExportLegacySimulationSnapshot(CPetriSimDoc& document);
}
