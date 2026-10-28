#pragma once
#include "ConflictTopology.h"

namespace hps::domain
{
enum class ConflictKind { Branch, Meet };
struct ConflictDecision
{
    ConflictKind kind;
    TransitionId visitedTransition;
    PlaceId place;
    std::size_t activeNormalArcCount;
    std::int64_t rawArcIndex;
    std::vector<TransitionId> disabled;
};
struct ConflictSelectionPlan
{
    std::vector<TransitionId> selectedInNetOrder;
    std::vector<ConflictDecision> decisions;
};
using ConflictSelectionResult = std::variant<ConflictSelectionPlan, std::vector<ValidationIssue>>;

// Explicit legacy compatibility policy: count active normal arcs, but select a
// raw adjacency index (including the known inactive/mixed/parallel-arc defects).
// conflictOrder is the caller's ordered pre-scan list; no pre-scan is inferred.
// choices supplies exactly one final RandLong(0, activeNormalArcCount) result
// per encountered conflict. No RNG, token changes, timing or UI mutation.
ConflictSelectionResult PlanLegacyConflictSelection(const NetSnapshot& snapshot,
    const ConflictTopology& topology, const std::vector<TransitionId>& conflictOrder,
    const std::vector<std::int64_t>& choices);
}
