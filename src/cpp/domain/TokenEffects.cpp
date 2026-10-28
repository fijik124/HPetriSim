#include "TokenEffects.h"
#include <limits>
#include <set>

namespace hps::domain
{
TokenEffectResult PlanLegacyTokenEffects(const NetSnapshot& snapshot,
    const std::vector<TransitionId>& selected)
{
    const auto eligibility = EvaluateLegacyEligibility(snapshot);
    if (const auto* issues = std::get_if<std::vector<ValidationIssue>>(&eligibility)) return *issues;
    std::map<TransitionId, bool> candidates;
    for (const auto& decision : std::get<std::vector<TransitionEligibility>>(eligibility))
        candidates.emplace(decision.id, std::holds_alternative<FireCandidate>(decision.state));
    std::set<TransitionId> selection;
    std::vector<ValidationIssue> issues;
    for (const auto id : selected)
    {
        if (!selection.insert(id).second)
            issues.push_back({ValidationCode::InvalidSelection, id.value, "A transition was selected more than once"});
        const auto candidate = candidates.find(id);
        if (candidate == candidates.end())
            issues.push_back({ValidationCode::InvalidSelection, id.value, "Selected transition does not exist"});
        else if (!candidate->second)
            issues.push_back({ValidationCode::InvalidSelection, id.value, "Selected transition is blocked or still waiting"});
    }
    if (!issues.empty()) return issues;

    TokenEffectPlan plan;
    plan.markingAfter = snapshot.marking;
    std::map<PlaceId, TokenCount> capacities;
    for (const auto& place : snapshot.net.places) capacities.emplace(place.id, place.capacity);
    for (const auto& transition : snapshot.net.transitions)
    {
        plan.consumedTokensAfter.emplace(transition.id, snapshot.transitionStates.at(transition.id).consumedTokens);
        if (selection.contains(transition.id)) plan.selectedInNetOrder.push_back(transition.id);
    }
    constexpr TokenCount maximum = std::numeric_limits<std::int32_t>::max();
    auto additionFits = [](TokenCount base, TokenCount increment) {
        return base <= maximum && increment <= maximum - base;
    };
    for (const auto& arc : snapshot.net.arcs)
    {
        const auto* transition = std::get_if<TransitionId>(&arc.to);
        if (!transition || !selection.contains(*transition) || arc.kind != ArcKind::Normal) continue;
        const auto place = std::get<PlaceId>(arc.from);
        auto& marking = plan.markingAfter.at(place);
        if (marking.tokens < arc.weight)
            return std::vector<ValidationIssue>{{ValidationCode::TokenUnderflow, place.value, "Selected transitions jointly consume more tokens than are available"}};
        auto& consumed = plan.consumedTokensAfter.at(*transition);
        if (!additionFits(consumed, arc.weight))
            return std::vector<ValidationIssue>{{ValidationCode::ArithmeticOverflow, transition->value, "Consumed-token statistics exceed the signed 32-bit legacy range"}};
        marking.tokens -= arc.weight;
        consumed += arc.weight;
        plan.movedTokens = true;
    }
    for (const auto& arc : snapshot.net.arcs)
    {
        const auto* transition = std::get_if<TransitionId>(&arc.from);
        if (!transition || !selection.contains(*transition) || arc.kind != ArcKind::Normal) continue;
        const auto place = std::get<PlaceId>(arc.to);
        auto& marking = plan.markingAfter.at(place);
        if (arc.weight > capacities.at(place) - marking.tokens)
            return std::vector<ValidationIssue>{{ValidationCode::CapacityExceeded, place.value, "Selected transitions jointly exceed output capacity"}};
        if (!additionFits(marking.producedTokens, arc.weight))
            return std::vector<ValidationIssue>{{ValidationCode::ArithmeticOverflow, place.value, "Produced-token statistics exceed the signed 32-bit legacy range"}};
        marking.tokens += arc.weight;
        marking.producedTokens += arc.weight;
        plan.movedTokens = true;
    }
    return plan;
}
}
