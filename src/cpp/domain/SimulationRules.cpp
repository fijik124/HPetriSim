#include "SimulationRules.h"
#include <limits>
#include <optional>

namespace hps::domain
{
EligibilityResult EvaluateLegacyEligibility(const NetSnapshot& snapshot)
{
    auto issues = Validate(snapshot);
    if (!issues.empty()) return issues;
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
    auto countInRange = [&](TokenCount value, auto id, const char* message) {
        if (value > static_cast<TokenCount>(maximum))
            issues.push_back({ValidationCode::UnsupportedLegacyRange, id.value, message});
    };
    std::map<PlaceId, TokenCount> capacities;
    for (const auto& place : snapshot.net.places)
    {
        countInRange(place.capacity, place.id, "Capacity exceeds the signed 32-bit legacy rule range");
        countInRange(snapshot.marking.at(place.id).tokens, place.id, "Marking exceeds the signed 32-bit legacy rule range");
        capacities.emplace(place.id, place.capacity);
    }
    for (const auto& arc : snapshot.net.arcs)
        countInRange(arc.weight, arc.id, "Weight exceeds the signed 32-bit legacy rule range");
    for (const auto& [id, state] : snapshot.transitionStates)
        if (state.remainingDelayMs < minimum || state.remainingDelayMs > maximum)
            issues.push_back({ValidationCode::UnsupportedLegacyRange, id.value, "Remaining delay exceeds the signed 32-bit legacy rule range"});
    if (!issues.empty()) return issues;

    struct ArcIndices { std::vector<std::size_t> inputs, outputs; };
    std::map<TransitionId, ArcIndices> index;
    for (const auto& transition : snapshot.net.transitions) index.emplace(transition.id, ArcIndices{});
    for (std::size_t position = 0; position < snapshot.net.arcs.size(); ++position)
    {
        const auto& arc = snapshot.net.arcs[position];
        if (const auto* id = std::get_if<TransitionId>(&arc.to)) index.at(*id).inputs.push_back(position);
        else index.at(std::get<TransitionId>(arc.from)).outputs.push_back(position);
    }

    std::vector<TransitionEligibility> result;
    result.reserve(snapshot.net.transitions.size());
    for (const auto& transition : snapshot.net.transitions)
    {
        std::optional<ArcBlocked> blocked;
        const auto& connected = index.at(transition.id);
        for (const auto position : connected.inputs)
        {
            const auto& arc = snapshot.net.arcs[position];
            const auto tokens = snapshot.marking.at(std::get<PlaceId>(arc.from)).tokens;
            if (arc.kind == ArcKind::Inhibitor)
            {
                if (tokens >= arc.weight) blocked = ArcBlocked{arc.id, BlockReason::InputInhibitorThreshold};
            }
            else if (tokens < arc.weight) blocked = ArcBlocked{arc.id, BlockReason::InsufficientInputTokens};
            if (blocked) break;
        }
        if (!blocked)
            for (const auto position : connected.outputs)
            {
                const auto& arc = snapshot.net.arcs[position];
                const auto place = std::get<PlaceId>(arc.to);
                const auto tokens = snapshot.marking.at(place).tokens;
                const auto capacity = capacities.at(place);
                if (arc.kind == ArcKind::Inhibitor)
                {
                    // Preserve the legacy (tokens - capacity) >= weight test.
                    // The guard prevents unsigned underflow; valid markings
                    // make this false even at capacity, matching the old code.
                    if (tokens >= capacity && tokens - capacity >= arc.weight)
                        blocked = ArcBlocked{arc.id, BlockReason::OutputInhibitorThreshold};
                }
                else if (capacity - tokens < arc.weight)
                    blocked = ArcBlocked{arc.id, BlockReason::InsufficientOutputCapacity};
                if (blocked) break;
            }
        if (blocked) result.push_back({transition.id, *blocked});
        else if (const auto delay = snapshot.transitionStates.at(transition.id).remainingDelayMs; delay > 0)
            result.push_back({transition.id, DelayPending{delay}});
        else result.push_back({transition.id, FireCandidate{}});
    }
    return result;
}
}
