#pragma once
#include "PetriNet.h"

namespace hps::domain
{
struct NodeAdjacency
{
    std::vector<ArcId> incoming;
    std::vector<ArcId> outgoing;
};
// Node key order is not selection order. Transition order stays in PetriNet;
// each vector preserves the actual legacy endpoint's connector order.
using ConflictTopology = std::map<NodeId, NodeAdjacency>;

// Requires an entry for every node, including isolated nodes, and each arc once
// at its source/outgoing and destination/incoming. Does not repair or sort.
std::vector<ValidationIssue> ValidateConflictTopology(
    const NetSnapshot& snapshot, const ConflictTopology& topology);
}
