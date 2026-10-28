#include "ConflictSelection.h"
#include "SimulationRules.h"
#include <limits>
#include <set>

namespace hps::domain
{
ConflictSelectionResult PlanLegacyConflictSelection(const NetSnapshot& snapshot,
    const ConflictTopology& topology, const std::vector<TransitionId>& conflictOrder,
    const std::vector<std::int64_t>& choices)
{
    auto issues = ValidateConflictTopology(snapshot, topology);
    if (!issues.empty()) return issues;
    const auto evaluated = EvaluateLegacyEligibility(snapshot);
    if (const auto* errors = std::get_if<std::vector<ValidationIssue>>(&evaluated)) return *errors;
    std::map<TransitionId, bool> active;
    std::map<TransitionId, bool> visitable;
    for (const auto& eligibility : std::get<std::vector<TransitionEligibility>>(evaluated))
    {
        active.emplace(eligibility.id, std::holds_alternative<FireCandidate>(eligibility.state));
        visitable.emplace(eligibility.id, true);
    }
    std::set<TransitionId> seen;
    for (const auto id : conflictOrder)
        if (!active.contains(id) || !seen.insert(id).second)
            issues.push_back({ValidationCode::InvalidSelection, id.value, "Conflict order contains an unknown or duplicate transition"});
    if (!issues.empty()) return issues;
    std::map<ArcId, const Arc*> arcs;
    for (const auto& arc : snapshot.net.arcs) arcs.emplace(arc.id, &arc);
    ConflictSelectionPlan plan;
    std::size_t nextChoice = 0;
    auto resolve = [&](TransitionId current, PlaceId place, ConflictKind kind) {
        const bool branch = kind == ConflictKind::Branch;
        const auto& adjacency = topology.at(place);
        const auto& peers = branch ? adjacency.outgoing : adjacency.incoming;
        auto transitionOf = [&](ArcId id) {
            const auto& arc = *arcs.at(id);
            return std::get<TransitionId>(branch ? arc.to : arc.from);
        };
        std::size_t count = 0;
        for (const auto id : peers)
            if (arcs.at(id)->kind == ArcKind::Normal && active.at(transitionOf(id))) ++count;
        if (count <= 1) return true;
        if (count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        {
            issues.push_back({ValidationCode::UnsupportedLegacyRange, place.value, "Active normal arc count exceeds the signed 32-bit legacy range"});
            return false;
        }
        if (nextChoice == choices.size() || choices[nextChoice] < 0 ||
            static_cast<std::uint64_t>(choices[nextChoice]) >= count)
        {
            issues.push_back({ValidationCode::InvalidConflictChoice, place.value, "Conflict needs an explicit choice in [0, active normal arc count)"});
            return false;
        }
        const auto choice = choices[nextChoice++];
        ConflictDecision decision{kind, current, place, count, choice, {}};
        for (std::size_t index = 0; index < peers.size(); ++index)
        {
            const auto id = transitionOf(peers[index]);
            visitable.at(id) = false;
            if (active.at(id) && index != static_cast<std::uint64_t>(choice))
            {
                active.at(id) = false;
                decision.disabled.push_back(id);
            }
        }
        plan.decisions.push_back(std::move(decision));
        return true;
    };
    for (const auto id : conflictOrder)
    {
        if (!active.at(id) || !visitable.at(id)) continue;
        const auto& adjacency = topology.at(id);
        // Entry guard is deliberately not repeated after losing a conflict.
        for (const auto arc : adjacency.incoming)
            if (!resolve(id, std::get<PlaceId>(arcs.at(arc)->from), ConflictKind::Branch)) return issues;
        for (const auto arc : adjacency.outgoing)
            if (!resolve(id, std::get<PlaceId>(arcs.at(arc)->to), ConflictKind::Meet)) return issues;
    }
    if (nextChoice != choices.size())
        return std::vector<ValidationIssue>{{ValidationCode::InvalidConflictChoice, 0, "Unused conflict choices remain"}};
    for (const auto& transition : snapshot.net.transitions)
        if (active.at(transition.id)) plan.selectedInNetOrder.push_back(transition.id);
    return plan;
}
}
