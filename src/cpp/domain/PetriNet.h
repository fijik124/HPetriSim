#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace hps::domain
{
template<class Tag> struct Id
{
    std::uint32_t value = 0;
    auto operator<=>(const Id&) const = default;
};
using PlaceId = Id<struct PlaceTag>;
using TransitionId = Id<struct TransitionTag>;
using ArcId = Id<struct ArcTag>;
using NodeId = std::variant<PlaceId, TransitionId>;
using TokenCount = std::uint64_t;

enum class ArcKind { Normal, Inhibitor, Test };
enum class TimeMode { Immediate, FixedDelay, Exponential, Uniform };

struct Place
{
    PlaceId id;
    std::u16string name;
    TokenCount capacity = 1;
    TokenCount initialTokens = 0;
};
struct Transition
{
    TransitionId id;
    std::u16string name;
    TimeMode mode = TimeMode::Immediate;
    std::int64_t initialDelayMs = 0;
    std::int64_t rangeMaxDelayMs = 0;
};
struct Arc
{
    ArcId id;
    std::u16string name;
    NodeId from;
    NodeId to;
    ArcKind kind = ArcKind::Normal;
    TokenCount weight = 1;
};
struct PetriNet
{
    // Preserve legacy order: conflict selection currently depends on it.
    std::vector<Place> places;
    std::vector<Transition> transitions;
    std::vector<Arc> arcs;
};
struct PlaceState
{
    TokenCount tokens = 0;
    TokenCount producedTokens = 0;
};
using Marking = std::map<PlaceId, PlaceState>;
struct TransitionState
{
    // Signed: the legacy countdown can overshoot below zero.
    std::int64_t remainingDelayMs = 0;
    TokenCount consumedTokens = 0;
    bool active = false;
    bool enabled = false;
};
struct NetSnapshot
{
    PetriNet net;
    Marking marking;
    std::map<TransitionId, TransitionState> transitionStates;
};

enum class ValidationCode
{
    InvalidId, DuplicateId, InvalidCapacity, CapacityExceeded,
    InvalidWeight, InvalidTiming, InvalidEnum, MissingEndpoint,
    NonBipartiteArc, MissingState, UnexpectedState,
    InvalidObject, DisconnectedArc, ForeignEndpoint, StaleReference, SnapshotUnavailable,
    UnsupportedLegacyRange
};
struct ValidationIssue
{
    ValidationCode code;
    std::uint32_t objectId;
    std::string message;
};

// Does not repair, normalize or mutate the snapshot.
std::vector<ValidationIssue> Validate(const NetSnapshot& snapshot);
}
