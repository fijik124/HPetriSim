#pragma once
#include "TokenEffects.h"

namespace hps::domain
{
struct StepClock
{
    std::int64_t timeMs = 0;
    std::int64_t steps = 0;
    std::int64_t sampleTimeMs = 1;
    std::int64_t timeLimitMs = 1000;
    std::int64_t stepLimit = 1000;
};
struct TransitionTiming
{
    std::int64_t remainingDelayMs;
    bool active;
    bool enabled;
};
struct StopFlags
{
    bool deadlock = false;
    bool timeLimitReached = false;
    bool stepLimitReached = false;
    bool Locked() const { return deadlock || timeLimitReached || stepLimitReached; }
};
using SampledDelays = std::map<TransitionId, std::int64_t>;
struct TimingPlan
{
    std::map<TransitionId, TransitionTiming> transitionsAfter;
    StepClock clockAfter;
    StopFlags stop;
};
using TimingResult = std::variant<TimingPlan, std::vector<ValidationIssue>>;

// Samples are final resampled delays for selected stochastic transitions only.
// This function does not generate randomness or mutate input. Selection remains
// the caller's responsibility; token effects are validated before timing.
TimingResult PlanLegacyTiming(const NetSnapshot& snapshot,
    const std::vector<TransitionId>& selected, const StepClock& clock,
    const SampledDelays& samples = {});
}
