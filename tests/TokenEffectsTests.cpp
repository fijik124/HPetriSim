#include "domain/TokenEffects.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Token-effect headers must not import MFC or Windows headers
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
    net.net.transitions = {{TransitionId{3}, u"T", TimeMode::Immediate, 0, 0}};
    net.net.arcs = {{ArcId{4}, u"In", PlaceId{1}, TransitionId{3}, ArcKind::Normal, 2},
                    {ArcId{5}, u"Out", TransitionId{3}, PlaceId{2}, ArcKind::Normal, 3}};
    net.marking = {{PlaceId{1}, {4, 7}}, {PlaceId{2}, {1, 8}}};
    net.transitionStates = {{TransitionId{3}, {0, 6, false, false}}};
    return net;
}
TokenEffectPlan Plan(const NetSnapshot& net, const std::vector<TransitionId>& selected = {TransitionId{3}})
{
    const auto result = PlanLegacyTokenEffects(net, selected);
    REQUIRE(std::holds_alternative<TokenEffectPlan>(result));
    return std::get<TokenEffectPlan>(result);
}
bool HasError(const NetSnapshot& net, const std::vector<TransitionId>& selected, ValidationCode code)
{
    const auto result = PlanLegacyTokenEffects(net, selected);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    const auto& issues = std::get<std::vector<ValidationIssue>>(result);
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == code; });
}
}

TEST_CASE("Weighted firing plans independent marking and counter values", "[token-effects]")
{
    const auto net = Net();
    const auto plan = Plan(net);
    CHECK(plan.markingAfter.at(PlaceId{1}).tokens == 2);
    CHECK(plan.markingAfter.at(PlaceId{2}).tokens == 4);
    CHECK(plan.markingAfter.at(PlaceId{1}).producedTokens == 7);
    CHECK(plan.markingAfter.at(PlaceId{2}).producedTokens == 11);
    CHECK(plan.consumedTokensAfter.at(TransitionId{3}) == 8);
    CHECK(plan.movedTokens);
    CHECK(net.marking.at(PlaceId{1}).tokens == 4);
    CHECK(net.marking.at(PlaceId{2}).producedTokens == 8);
    CHECK(net.transitionStates.at(TransitionId{3}).consumedTokens == 6);
}
TEST_CASE("Read and inhibitor inputs do not consume or increment transition statistics", "[token-effects]")
{
    for (auto kind : {ArcKind::Test, ArcKind::Inhibitor})
    {
        auto net = Net();
        net.net.arcs[0].kind = kind;
        net.net.arcs[0].weight = kind == ArcKind::Test ? 2 : 5;
        const auto plan = Plan(net);
        CHECK(plan.markingAfter.at(PlaceId{1}).tokens == 4);
        CHECK(plan.consumedTokensAfter.at(TransitionId{3}) == 6);
        CHECK(plan.markingAfter.at(PlaceId{2}).tokens == 4);
    }
}
TEST_CASE("Read and inhibitor outputs do not produce tokens or statistics", "[token-effects]")
{
    for (auto kind : {ArcKind::Test, ArcKind::Inhibitor})
    {
        auto net = Net();
        net.net.arcs[1].kind = kind;
        const auto plan = Plan(net);
        CHECK(plan.markingAfter.at(PlaceId{1}).tokens == 2);
        CHECK(plan.markingAfter.at(PlaceId{2}).tokens == 1);
        CHECK(plan.markingAfter.at(PlaceId{2}).producedTokens == 8);
    }
}
TEST_CASE("Empty selection returns identity effects", "[token-effects]")
{
    const auto plan = Plan(Net(), {});
    CHECK(plan.markingAfter.at(PlaceId{1}).tokens == 4);
    CHECK(plan.markingAfter.at(PlaceId{2}).tokens == 1);
    CHECK(plan.consumedTokensAfter.at(TransitionId{3}) == 6);
    CHECK(plan.selectedInNetOrder.empty());
    CHECK_FALSE(plan.movedTokens);
}
TEST_CASE("Unknown duplicate and noneligible selections are diagnosed", "[token-effects]")
{
    auto net = Net();
    CHECK(HasError(net, {TransitionId{99}}, ValidationCode::InvalidSelection));
    CHECK(HasError(net, {TransitionId{3}, TransitionId{3}}, ValidationCode::InvalidSelection));
    net.marking.at(PlaceId{1}).tokens = 0;
    CHECK(HasError(net, {TransitionId{3}}, ValidationCode::InvalidSelection));
    net = Net();
    net.transitionStates.at(TransitionId{3}).remainingDelayMs = 1;
    CHECK(HasError(net, {TransitionId{3}}, ValidationCode::InvalidSelection));
}
TEST_CASE("Invalid snapshots do not produce an effect plan", "[token-effects]")
{
    auto net = Net();
    net.marking.erase(PlaceId{1});
    CHECK(HasError(net, {}, ValidationCode::MissingState));
}
TEST_CASE("Joint input shortage is rejected before any output restores tokens", "[token-effects]")
{
    auto net = Net();
    net.marking.at(PlaceId{1}).tokens = 2;
    net.net.places[0].capacity = 4;
    net.net.arcs[1].to = PlaceId{1};
    net.net.arcs[1].weight = 2;
    net.net.transitions.push_back({TransitionId{6}, u"Other", TimeMode::Immediate, 0, 0});
    net.transitionStates.emplace(TransitionId{6}, TransitionState{});
    net.net.arcs.push_back({ArcId{7}, u"Other in", PlaceId{1}, TransitionId{6}, ArcKind::Normal, 2});
    net.net.arcs.push_back({ArcId{8}, u"Other out", TransitionId{6}, PlaceId{1}, ArcKind::Normal, 2});
    CHECK(HasError(net, {TransitionId{3}, TransitionId{6}}, ValidationCode::TokenUnderflow));
    CHECK(net.marking.at(PlaceId{1}).tokens == 2);
}
TEST_CASE("Joint output overflow is rejected despite individual eligibility", "[token-effects]")
{
    auto net = Net();
    net.net.transitions.push_back({TransitionId{6}, u"Source", TimeMode::Immediate, 0, 0});
    net.transitionStates.emplace(TransitionId{6}, TransitionState{});
    net.net.arcs.push_back({ArcId{7}, u"Source out", TransitionId{6}, PlaceId{2}, ArcKind::Normal, 7});
    CHECK(HasError(net, {TransitionId{3}, TransitionId{6}}, ValidationCode::CapacityExceeded));
    CHECK(net.marking.at(PlaceId{1}).tokens == 4);
    CHECK(net.marking.at(PlaceId{2}).tokens == 1);
}
TEST_CASE("Statistics cannot cross the signed legacy boundary", "[token-effects]")
{
    auto net = Net();
    net.transitionStates.at(TransitionId{3}).consumedTokens = std::numeric_limits<std::int32_t>::max();
    CHECK(HasError(net, {TransitionId{3}}, ValidationCode::ArithmeticOverflow));
    net = Net();
    net.marking.at(PlaceId{2}).producedTokens = std::numeric_limits<std::int32_t>::max();
    CHECK(HasError(net, {TransitionId{3}}, ValidationCode::ArithmeticOverflow));
}
TEST_CASE("Statistics can reach the signed legacy boundary exactly", "[token-effects]")
{
    auto net = Net();
    constexpr TokenCount maximum = std::numeric_limits<std::int32_t>::max();
    net.transitionStates.at(TransitionId{3}).consumedTokens = maximum - 2;
    net.marking.at(PlaceId{2}).producedTokens = maximum - 3;
    const auto plan = Plan(net);
    CHECK(plan.consumedTokensAfter.at(TransitionId{3}) == maximum);
    CHECK(plan.markingAfter.at(PlaceId{2}).producedTokens == maximum);
}
TEST_CASE("Self-loops report transfers even when their marking is unchanged", "[token-effects]")
{
    auto net = Net();
    net.net.arcs[1].to = PlaceId{1};
    net.net.arcs[1].weight = 2;
    const auto plan = Plan(net);
    CHECK(plan.markingAfter.at(PlaceId{1}).tokens == 4);
    CHECK(plan.markingAfter.at(PlaceId{1}).producedTokens == 9);
    CHECK(plan.movedTokens);
    net.net.places[0].capacity = 4;
    CHECK(HasError(net, {TransitionId{3}}, ValidationCode::InvalidSelection));
}
TEST_CASE("Selected transitions with no normal arcs have no token transfers", "[token-effects]")
{
    auto net = Net();
    net.net.arcs[0].kind = ArcKind::Test;
    net.net.arcs[1].kind = ArcKind::Test;
    const auto plan = Plan(net);
    CHECK_FALSE(plan.movedTokens);
    CHECK(plan.markingAfter.at(PlaceId{1}).tokens == 4);
    CHECK(plan.markingAfter.at(PlaceId{2}).tokens == 1);
    net.net.arcs.clear();
    CHECK_FALSE(Plan(net).movedTokens);
}
TEST_CASE("Selection order cannot change the canonical transition order", "[token-effects]")
{
    auto net = Net();
    net.net.transitions.push_back({TransitionId{6}, u"Isolated", TimeMode::Immediate, 0, 0});
    net.transitionStates.emplace(TransitionId{6}, TransitionState{0, 17, false, false});
    const auto plan = Plan(net, {TransitionId{6}, TransitionId{3}});
    const std::vector<TransitionId> expected = {TransitionId{3}, TransitionId{6}};
    CHECK(plan.selectedInNetOrder == expected);
    CHECK(plan.consumedTokensAfter.at(TransitionId{6}) == 17);
}
TEST_CASE("Read arcs observe initial marking rather than consumed marking", "[token-effects]")
{
    auto net = Net();
    net.marking.at(PlaceId{1}).tokens = 2;
    net.net.arcs.push_back({ArcId{6}, u"Read", PlaceId{1}, TransitionId{3}, ArcKind::Test, 2});
    const auto plan = Plan(net);
    CHECK(plan.markingAfter.at(PlaceId{1}).tokens == 0);
    CHECK(plan.consumedTokensAfter.at(TransitionId{3}) == 8);
}
TEST_CASE("Untouched statistics remain intact without narrowing", "[token-effects]")
{
    auto net = Net();
    net.marking.at(PlaceId{1}).producedTokens = std::numeric_limits<TokenCount>::max();
    net.net.transitions.push_back({TransitionId{6}, u"Unselected", TimeMode::Immediate, 0, 0});
    net.transitionStates.emplace(TransitionId{6}, TransitionState{0, std::numeric_limits<TokenCount>::max(), false, false});
    const auto plan = Plan(net);
    CHECK(plan.markingAfter.at(PlaceId{1}).producedTokens == std::numeric_limits<TokenCount>::max());
    CHECK(plan.consumedTokensAfter.at(TransitionId{6}) == std::numeric_limits<TokenCount>::max());
}
