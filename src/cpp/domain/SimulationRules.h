#pragma once
#include "PetriNet.h"

namespace hps::domain
{
enum class BlockReason
{
    InsufficientInputTokens, InputInhibitorThreshold,
    InsufficientOutputCapacity, OutputInhibitorThreshold
};
struct ArcBlocked
{
    ArcId arc;
    BlockReason reason;
};
struct DelayPending { std::int64_t remainingDelayMs; };
struct FireCandidate {};
using EligibilityState = std::variant<ArcBlocked, DelayPending, FireCandidate>;
struct TransitionEligibility
{
    TransitionId id;
    EligibilityState state;
};
using EligibilityResult = std::variant<std::vector<TransitionEligibility>, std::vector<ValidationIssue>>;

// Legacy pre-conflict predicates only. No firing, RNG, timer decrement, mutation
// or UI notification. Invalid or out-of-legacy-range inputs return diagnostics.
EligibilityResult EvaluateLegacyEligibility(const NetSnapshot& snapshot);
}
