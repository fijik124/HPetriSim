#include "ConflictTopology.h"
#include <set>

namespace hps::domain
{
std::vector<ValidationIssue> ValidateConflictTopology(
    const NetSnapshot& snapshot, const ConflictTopology& topology)
{
    auto issues = Validate(snapshot);
    if (!issues.empty()) return issues; // Endpoint/ID lookup must be unambiguous.
    std::set<NodeId> nodes;
    for (const auto& place : snapshot.net.places) nodes.insert(place.id);
    for (const auto& transition : snapshot.net.transitions) nodes.insert(transition.id);
    std::map<ArcId, const Arc*> arcs;
    for (const auto& arc : snapshot.net.arcs) arcs.emplace(arc.id, &arc);
    auto nodeValue = [](const NodeId& node) {
        return std::visit([](auto id) { return id.value; }, node);
    };
    for (const auto& node : nodes)
        if (!topology.contains(node))
            issues.push_back({ValidationCode::MissingAdjacency, nodeValue(node), "Node adjacency is missing"});
    for (const auto& [node, adjacency] : topology)
    {
        if (!nodes.contains(node))
            issues.push_back({ValidationCode::UnexpectedAdjacency, nodeValue(node), "Adjacency refers to an unknown node"});
        auto check = [&](const std::vector<ArcId>& entries, bool incoming) {
            std::set<ArcId> seen;
            for (const auto id : entries)
            {
                if (!seen.insert(id).second)
                    issues.push_back({ValidationCode::DuplicateArcReference, id.value, "Arc is repeated in a node adjacency list"});
                const auto found = arcs.find(id);
                if (found == arcs.end())
                    issues.push_back({ValidationCode::InvalidArcReference, id.value, "Adjacency refers to an unknown arc"});
                else if ((incoming ? found->second->to : found->second->from) != node)
                    issues.push_back({ValidationCode::InvalidArcReference, id.value, "Arc direction or endpoint disagrees with adjacency"});
            }
        };
        check(adjacency.incoming, true);
        check(adjacency.outgoing, false);
    }
    for (const auto& arc : snapshot.net.arcs)
    {
        auto contains = [&](const NodeId& node, bool incoming) {
            const auto found = topology.find(node);
            if (found == topology.end()) return false;
            const auto& entries = incoming ? found->second.incoming : found->second.outgoing;
            for (const auto id : entries) if (id == arc.id) return true;
            return false;
        };
        if (!contains(arc.from, false) || !contains(arc.to, true))
            issues.push_back({ValidationCode::MissingArcReference, arc.id.value, "Arc is missing from its source or destination adjacency"});
    }
    return issues;
}
}
