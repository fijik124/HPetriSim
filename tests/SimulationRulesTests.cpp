#include "domain/SimulationRules.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Rule headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>

using namespace hps::domain;
namespace
{
NetSnapshot Net()
{
    NetSnapshot net;
    net.net.places = {{PlaceId{1}, u"Input", 10, 0}, {PlaceId{2}, u"Output", 10, 0}};
    net.net.transitions = {{TransitionId{3}, u"T", TimeMode::FixedDelay, 2, 0}};
    net.net.arcs = {{ArcId{4}, u"In", PlaceId{1}, TransitionId{3}, ArcKind::Normal, 2},
                    {ArcId{5}, u"Out", TransitionId{3}, PlaceId{2}, ArcKind::Normal, 3}};
    net.marking = {{PlaceId{1}, {2, 7}}, {PlaceId{2}, {7, 8}}};
    net.transitionStates = {{TransitionId{3}, {0, 6, false, false}}};
    return net;
}
EligibilityState State(const NetSnapshot& net)
{
    const auto result = EvaluateLegacyEligibility(net);
    REQUIRE(std::holds_alternative<std::vector<TransitionEligibility>>(result));
    const auto& states = std::get<std::vector<TransitionEligibility>>(result);
    REQUIRE(states.size() == 1);
    return states[0].state;
}
bool HasError(const NetSnapshot& net, ValidationCode code)
{
    const auto result = EvaluateLegacyEligibility(net);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    const auto& issues = std::get<std::vector<ValidationIssue>>(result);
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == code; });
}
}

TEST_CASE("Weighted exact fits become candidates without changing input", "[rules]")
{
    const auto net = Net();
    CHECK(std::holds_alternative<FireCandidate>(State(net)));
    CHECK(net.marking.at(PlaceId{1}).tokens == 2);
    CHECK(net.marking.at(PlaceId{2}).tokens == 7);
    CHECK(net.marking.at(PlaceId{2}).producedTokens == 8);
    CHECK(net.transitionStates.at(TransitionId{3}).consumedTokens == 6);
}
TEST_CASE("Normal and test input arcs share the token threshold", "[rules]")
{
    for (auto kind : {ArcKind::Normal, ArcKind::Test})
    {
        auto net = Net();
        net.net.arcs[0].kind = kind;
        CHECK(std::holds_alternative<FireCandidate>(State(net)));
        net.marking.at(PlaceId{1}).tokens = 1;
        const auto state = State(net);
        REQUIRE(std::holds_alternative<ArcBlocked>(state));
        CHECK(std::get<ArcBlocked>(state).arc == ArcId{4});
        CHECK(std::get<ArcBlocked>(state).reason == BlockReason::InsufficientInputTokens);
    }
}
TEST_CASE("Input inhibitors require a strict less-than comparison", "[rules]")
{
    auto net = Net();
    net.net.arcs[0].kind = ArcKind::Inhibitor;
    net.marking.at(PlaceId{1}).tokens = 1;
    CHECK(std::holds_alternative<FireCandidate>(State(net)));
    net.marking.at(PlaceId{1}).tokens = 2;
    CHECK(std::get<ArcBlocked>(State(net)).reason == BlockReason::InputInhibitorThreshold);
}
TEST_CASE("Normal and test output arcs require enough free capacity", "[rules]")
{
    for (auto kind : {ArcKind::Normal, ArcKind::Test})
    {
        auto net = Net();
        net.net.arcs[1].kind = kind;
        CHECK(std::holds_alternative<FireCandidate>(State(net)));
        net.marking.at(PlaceId{2}).tokens = 8;
        const auto blocked = std::get<ArcBlocked>(State(net));
        CHECK(blocked.arc == ArcId{5});
        CHECK(blocked.reason == BlockReason::InsufficientOutputCapacity);
    }
}
TEST_CASE("Legacy output inhibitors permit a full valid place", "[rules]")
{
    auto net = Net();
    net.net.arcs[1].kind = ArcKind::Inhibitor;
    for (auto tokens : {0ULL, 7ULL, 10ULL})
    {
        net.marking.at(PlaceId{2}).tokens = tokens;
        CHECK(std::holds_alternative<FireCandidate>(State(net)));
    }
}
TEST_CASE("Delay alone distinguishes waiting from a firing candidate", "[rules]")
{
    for (auto mode : {TimeMode::Immediate, TimeMode::FixedDelay, TimeMode::Exponential, TimeMode::Uniform})
    {
        auto net = Net();
        net.net.transitions[0].mode = mode;
        net.net.transitions[0].rangeMaxDelayMs = 7;
        net.transitionStates.at(TransitionId{3}).remainingDelayMs = 2;
        CHECK(std::get<DelayPending>(State(net)).remainingDelayMs == 2);
        net.transitionStates.at(TransitionId{3}).remainingDelayMs = 0;
        CHECK(std::holds_alternative<FireCandidate>(State(net)));
        net.transitionStates.at(TransitionId{3}).remainingDelayMs = -1;
        CHECK(std::holds_alternative<FireCandidate>(State(net)));
    }
}
TEST_CASE("Arc blockers take precedence over a pending delay", "[rules]")
{
    auto net = Net();
    net.transitionStates.at(TransitionId{3}).remainingDelayMs = 2;
    net.marking.at(PlaceId{1}).tokens = 0;
    CHECK(std::holds_alternative<ArcBlocked>(State(net)));
}
TEST_CASE("Input blockers are evaluated before outputs even if arc storage is reversed", "[rules]")
{
    auto net = Net();
    net.marking.at(PlaceId{1}).tokens = 0;
    net.marking.at(PlaceId{2}).tokens = 10;
    std::reverse(net.net.arcs.begin(), net.net.arcs.end());
    CHECK(std::get<ArcBlocked>(State(net)).arc == ArcId{4});
}
TEST_CASE("Legacy full self-loops remain blocked before input consumption", "[rules]")
{
    auto net = Net();
    net.net.arcs[1].to = PlaceId{1};
    net.net.arcs[1].weight = 2;
    net.net.places[0].capacity = 2;
    CHECK(std::holds_alternative<ArcBlocked>(State(net)));
}
TEST_CASE("Candidates retain transition order including isolated transitions", "[rules]")
{
    auto net = Net();
    net.net.transitions.insert(net.net.transitions.begin(), {TransitionId{9}, u"Isolated", TimeMode::Immediate, 0, 0});
    net.transitionStates.emplace(TransitionId{9}, TransitionState{});
    auto result = EvaluateLegacyEligibility(net);
    REQUIRE(std::holds_alternative<std::vector<TransitionEligibility>>(result));
    const auto& states = std::get<std::vector<TransitionEligibility>>(result);
    REQUIRE(states.size() == 2);
    CHECK(states[0].id == TransitionId{9});
    CHECK(states[1].id == TransitionId{3});
    CHECK(std::holds_alternative<FireCandidate>(states[0].state));
}
TEST_CASE("Empty nets produce an empty eligibility list", "[rules]")
{
    const auto result = EvaluateLegacyEligibility(NetSnapshot{});
    REQUIRE(std::holds_alternative<std::vector<TransitionEligibility>>(result));
    CHECK(std::get<std::vector<TransitionEligibility>>(result).empty());
}
TEST_CASE("Eligibility uses one marking rather than hypothetical earlier firings", "[rules]")
{
    auto net = Net();
    net.marking.at(PlaceId{2}).tokens = 0;
    net.net.transitions.push_back({TransitionId{6}, u"Next", TimeMode::Immediate, 0, 0});
    net.transitionStates.emplace(TransitionId{6}, TransitionState{});
    net.net.arcs.push_back({ArcId{7}, u"Next input", PlaceId{2}, TransitionId{6}, ArcKind::Normal, 1});
    const auto result = EvaluateLegacyEligibility(net);
    REQUIRE(std::holds_alternative<std::vector<TransitionEligibility>>(result));
    const auto& states = std::get<std::vector<TransitionEligibility>>(result);
    REQUIRE(states.size() == 2);
    CHECK(std::holds_alternative<FireCandidate>(states[0].state));
    CHECK(std::holds_alternative<ArcBlocked>(states[1].state));
    CHECK(net.marking.at(PlaceId{2}).tokens == 0);
}
TEST_CASE("Invalid snapshots return diagnostics instead of partial decisions", "[rules]")
{
    auto net = Net();
    net.marking.erase(PlaceId{1});
    CHECK(HasError(net, ValidationCode::MissingState));
    net = Net();
    net.net.arcs[0].weight = 0;
    CHECK(HasError(net, ValidationCode::InvalidWeight));
}
TEST_CASE("Values outside the legacy signed range are reported explicitly", "[rules]")
{
    auto net = Net();
    net.net.places[0].capacity = static_cast<TokenCount>(std::numeric_limits<std::int32_t>::max()) + 1;
    CHECK(HasError(net, ValidationCode::UnsupportedLegacyRange));
    net = Net();
    net.net.arcs[0].weight = static_cast<TokenCount>(std::numeric_limits<std::int32_t>::max()) + 1;
    CHECK(HasError(net, ValidationCode::UnsupportedLegacyRange));
    net = Net();
    net.transitionStates.at(TransitionId{3}).remainingDelayMs = std::numeric_limits<std::int64_t>::min();
    CHECK(HasError(net, ValidationCode::UnsupportedLegacyRange));
}
TEST_CASE("Legacy signed boundary is accepted without overflow", "[rules]")
{
    auto net = Net();
    net.net.places[0].capacity = std::numeric_limits<std::int32_t>::max();
    net.marking.at(PlaceId{1}).tokens = net.net.places[0].capacity;
    net.transitionStates.at(TransitionId{3}).remainingDelayMs = std::numeric_limits<std::int32_t>::min();
    CHECK(std::holds_alternative<FireCandidate>(State(net)));
}
