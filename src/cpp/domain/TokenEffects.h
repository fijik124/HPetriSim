#pragma once
#include "SimulationRules.h"

namespace hps::domain
{
struct TokenEffectPlan
{
    Marking markingAfter;
    std::map<TransitionId, TokenCount> consumedTokensAfter;
    std::vector<TransitionId> selectedInNetOrder;
    // True for any normal-arc transfer, even when the net marking is unchanged.
    bool movedTokens = false;
};
using TokenEffectResult = std::variant<TokenEffectPlan, std::vector<ValidationIssue>>;

// The caller selects conflict winners. This plans only their token effects,
// with all input consumption before output production. Input is never changed.
// Invalid selections or unsafe joint effects return errors, not a partial plan.
TokenEffectResult PlanLegacyTokenEffects(const NetSnapshot& snapshot,
    const std::vector<TransitionId>& selected);
}
