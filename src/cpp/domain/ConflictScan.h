#pragma once
#include "ConflictTopology.h"

namespace hps::domain
{
using ConflictScanResult = std::variant<std::vector<TransitionId>, std::vector<ValidationIssue>>;

// Structural pre-scan only. Includes all arc kinds and parallel arcs, ignores
// eligibility/resources, returns each qualifying transition once in net order.
// Validates the complete snapshot/topology; no flags, RNG or input are changed.
ConflictScanResult FindLegacyPotentialConflicts(
    const NetSnapshot& snapshot, const ConflictTopology& topology);
}
