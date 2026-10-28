#include "PetriNet.h"
#include <set>

namespace hps::domain
{
std::vector<ValidationIssue> Validate(const NetSnapshot& snapshot)
{
    std::vector<ValidationIssue> issues;
    auto report = [&](ValidationCode code, auto id, const char* message) {
        issues.push_back({code, id.value, message});
    };
    auto insertId = [&](auto& ids, auto id) {
        if (id.value == 0) report(ValidationCode::InvalidId, id, "Object ID must be nonzero");
        if (!ids.insert(id).second) report(ValidationCode::DuplicateId, id, "Duplicate object ID");
    };
    std::set<PlaceId> places;
    std::set<TransitionId> transitions;
    std::set<ArcId> arcs;
    for (const auto& place : snapshot.net.places)
    {
        insertId(places, place.id);
        if (place.capacity == 0) report(ValidationCode::InvalidCapacity, place.id, "Capacity must be positive");
        if (place.initialTokens > place.capacity) report(ValidationCode::CapacityExceeded, place.id, "Initial marking exceeds capacity");
        auto state = snapshot.marking.find(place.id);
        if (state == snapshot.marking.end()) report(ValidationCode::MissingState, place.id, "Place marking is missing");
        else if (state->second.tokens > place.capacity) report(ValidationCode::CapacityExceeded, place.id, "Current marking exceeds capacity");
    }
    for (const auto& transition : snapshot.net.transitions)
    {
        insertId(transitions, transition.id);
        if (transition.initialDelayMs < 0) report(ValidationCode::InvalidTiming, transition.id, "Initial delay must be nonnegative");
        switch (transition.mode)
        {
        case TimeMode::Immediate:
        case TimeMode::FixedDelay: break;
        case TimeMode::Exponential:
            if (transition.initialDelayMs == 0) report(ValidationCode::InvalidTiming, transition.id, "Exponential parameter must be positive");
            break;
        case TimeMode::Uniform:
            if (transition.rangeMaxDelayMs <= transition.initialDelayMs) report(ValidationCode::InvalidTiming, transition.id, "Uniform range must have an upper bound above its lower bound");
            break;
        default: report(ValidationCode::InvalidEnum, transition.id, "Unknown time mode"); break;
        }
        if (!snapshot.transitionStates.contains(transition.id)) report(ValidationCode::MissingState, transition.id, "Transition state is missing");
    }
    for (const auto& [id, state] : snapshot.marking)
    {
        (void)state;
        if (!places.contains(id)) report(ValidationCode::UnexpectedState, id, "Marking refers to an unknown place");
    }
    for (const auto& [id, state] : snapshot.transitionStates)
    {
        (void)state;
        if (!transitions.contains(id)) report(ValidationCode::UnexpectedState, id, "State refers to an unknown transition");
    }
    auto exists = [&](const NodeId& node) {
        if (auto id = std::get_if<PlaceId>(&node)) return places.contains(*id);
        return transitions.contains(std::get<TransitionId>(node));
    };
    for (const auto& arc : snapshot.net.arcs)
    {
        insertId(arcs, arc.id);
        if (!exists(arc.from) || !exists(arc.to)) report(ValidationCode::MissingEndpoint, arc.id, "Arc endpoint is absent from the net");
        if (arc.from.index() == arc.to.index()) report(ValidationCode::NonBipartiteArc, arc.id, "Arc must connect a place and a transition");
        if (arc.weight == 0) report(ValidationCode::InvalidWeight, arc.id, "Arc weight must be positive");
        switch (arc.kind)
        {
        case ArcKind::Normal:
        case ArcKind::Inhibitor:
        case ArcKind::Test: break;
        default: report(ValidationCode::InvalidEnum, arc.id, "Unknown arc kind"); break;
        }
    }
    return issues;
}
}
