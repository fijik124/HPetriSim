#include "ConflictScan.h"

namespace hps::domain
{
ConflictScanResult FindLegacyPotentialConflicts(
    const NetSnapshot& snapshot, const ConflictTopology& topology)
{
    auto issues = ValidateConflictTopology(snapshot, topology);
    if (!issues.empty()) return issues;
    std::map<ArcId, const Arc*> arcs;
    for (const auto& arc : snapshot.net.arcs) arcs.emplace(arc.id, &arc);
    std::vector<TransitionId> conflicts;
    for (const auto& transition : snapshot.net.transitions)
    {
        const auto& adjacency = topology.at(transition.id);
        bool conflict = false;
        for (const auto id : adjacency.incoming)
            if (topology.at(arcs.at(id)->from).outgoing.size() > 1)
            {
                conflict = true;
                break;
            }
        if (!conflict)
            for (const auto id : adjacency.outgoing)
                if (topology.at(arcs.at(id)->to).incoming.size() > 1)
                {
                    conflict = true;
                    break;
                }
        if (conflict) conflicts.push_back(transition.id);
    }
    return conflicts;
}
}
