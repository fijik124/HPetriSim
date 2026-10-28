#include "domain/ConflictSelection.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Selection headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>

using namespace hps::domain;
namespace
{
constexpr PlaceId place{1}, other{2};
constexpr TransitionId first{3}, second{4}, third{5};
struct Net
{
    NetSnapshot snapshot;
    ConflictTopology topology;
    explicit Net(bool meet = false)
    {
        snapshot.net.places = {{place, u"P", 10, 0}, {other, u"Q", 10, 0}};
        snapshot.marking = {{place, {2, 0}}, {other, {2, 0}}};
        topology.emplace(place, NodeAdjacency{});
        topology.emplace(other, NodeAdjacency{});
        Transition(first);
        Transition(second);
        if (meet) { Arc(first, place); Arc(second, place); }
        else { Arc(place, first); Arc(place, second); }
    }
    void Transition(TransitionId id)
    {
        snapshot.net.transitions.push_back({id, u"T"});
        snapshot.transitionStates.emplace(id, TransitionState{});
        topology.emplace(id, NodeAdjacency{});
    }
    ArcId Arc(NodeId from, NodeId to, ArcKind kind = ArcKind::Normal, TokenCount weight = 1)
    {
        ArcId id{static_cast<std::uint32_t>(snapshot.net.arcs.size() + 10)};
        snapshot.net.arcs.push_back({id, u"A", from, to, kind, weight});
        topology.at(from).outgoing.push_back(id);
        topology.at(to).incoming.push_back(id);
        return id;
    }
    ConflictSelectionResult Select(const std::vector<TransitionId>& order,
        const std::vector<std::int64_t>& choices) const
    {
        return PlanLegacyConflictSelection(snapshot, topology, order, choices);
    }
};
ConflictSelectionPlan Plan(const Net& net, const std::vector<TransitionId>& order,
    const std::vector<std::int64_t>& choices)
{
    const auto result = net.Select(order, choices);
    REQUIRE(std::holds_alternative<ConflictSelectionPlan>(result));
    return std::get<ConflictSelectionPlan>(result);
}
bool HasError(const Net& net, const std::vector<TransitionId>& order,
    const std::vector<std::int64_t>& choices, ValidationCode code)
{
    const auto result = net.Select(order, choices);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    const auto& issues = std::get<std::vector<ValidationIssue>>(result);
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == code; });
}
}

TEST_CASE("Explicit branch choices are repeatable and leave state unchanged", "[conflict-selection]")
{
    const Net net;
    for (std::int64_t choice : {0, 1})
    {
        const auto plan = Plan(net, {first, second}, {choice});
        CHECK(plan.selectedInNetOrder == std::vector<TransitionId>{choice == 0 ? first : second});
        REQUIRE(plan.decisions.size() == 1);
        const auto& decision = plan.decisions.front();
        CHECK(decision.kind == ConflictKind::Branch);
        CHECK(decision.visitedTransition == first);
        CHECK(decision.place == place);
        CHECK(decision.activeNormalArcCount == 2);
        CHECK(decision.rawArcIndex == choice);
        CHECK(decision.disabled == std::vector<TransitionId>{choice == 0 ? second : first});
        CHECK(Plan(net, {first, second}, {choice}).selectedInNetOrder == plan.selectedInNetOrder);
    }
    CHECK(net.snapshot.marking.at(place).tokens == 2);
    CHECK_FALSE(net.snapshot.transitionStates.at(first).active);
    CHECK(net.topology.at(place).outgoing == std::vector<ArcId>{ArcId{10}, ArcId{11}});
}
TEST_CASE("Meet choices use producer adjacency despite spare capacity", "[conflict-selection]")
{
    const Net net(true);
    const auto plan = Plan(net, {first, second}, {1});
    CHECK(plan.selectedInNetOrder == std::vector<TransitionId>{second});
    REQUIRE(plan.decisions.size() == 1);
    CHECK(plan.decisions.front().kind == ConflictKind::Meet);
}
TEST_CASE("Empty conflict order leaves all eligible candidates selected", "[conflict-selection]")
{
    const auto plan = Plan(Net{}, {}, {});
    CHECK(plan.selectedInNetOrder == std::vector<TransitionId>{first, second});
    CHECK(plan.decisions.empty());
    CHECK(std::holds_alternative<ConflictSelectionPlan>(PlanLegacyConflictSelection({}, {}, {}, {})));
}
TEST_CASE("Blocked and waiting transitions never compete", "[conflict-selection]")
{
    Net net;
    SECTION("Blocked") { net.snapshot.net.arcs.front().weight = 3; }
    SECTION("Waiting") { net.snapshot.transitionStates.at(first).remainingDelayMs = 1; }
    const auto plan = Plan(net, {first, second}, {});
    CHECK(plan.selectedInNetOrder == std::vector<TransitionId>{second});
    CHECK(plan.decisions.empty());
}
TEST_CASE("Conflict order rejects duplicate and unknown transitions", "[conflict-selection]")
{
    const Net net;
    CHECK(HasError(net, {TransitionId{99}}, {}, ValidationCode::InvalidSelection));
    CHECK(HasError(net, {first, first}, {0}, ValidationCode::InvalidSelection));
}
TEST_CASE("Conflict choices require exact consumption and valid bounds", "[conflict-selection]")
{
    const Net net;
    for (const auto& choices : std::vector<std::vector<std::int64_t>>{
        {}, {-1}, {2}, {std::numeric_limits<std::int64_t>::max()}, {0, 0}})
        CHECK(HasError(net, {first, second}, choices, ValidationCode::InvalidConflictChoice));
    CHECK(HasError(net, {}, {0}, ValidationCode::InvalidConflictChoice));
}
TEST_CASE("Raw index compatibility can discard all active candidates", "[conflict-selection][legacy-defect]")
{
    Net net;
    net.Transition(third);
    net.snapshot.transitionStates.at(third).remainingDelayMs = 1;
    const auto extra = net.Arc(place, third);
    net.topology.at(place).outgoing = {extra, ArcId{10}, ArcId{11}};
    const auto plan = Plan(net, {first, second, third}, {0});
    CHECK(plan.selectedInNetOrder.empty());
    CHECK(plan.decisions.front().disabled == std::vector<TransitionId>{first, second});
}
TEST_CASE("Mixed-kind raw index compatibility retains a reader", "[conflict-selection][legacy-defect]")
{
    for (auto kind : {ArcKind::Test, ArcKind::Inhibitor})
    {
        Net net;
        net.Transition(third);
        const auto extra = net.Arc(place, third, kind, kind == ArcKind::Test ? 1 : 3);
        net.topology.at(place).outgoing = {extra, ArcId{10}, ArcId{11}};
        const auto plan = Plan(net, {third, first, second}, {0});
        CHECK(plan.selectedInNetOrder == std::vector<TransitionId>{third});
        CHECK(plan.decisions.front().activeNormalArcCount == 2);
    }
}
TEST_CASE("Parallel arc compatibility can disable the only transition", "[conflict-selection][legacy-defect]")
{
    Net net;
    net.snapshot.net.arcs.back().to = first;
    net.topology.at(first).incoming.push_back(ArcId{11});
    net.topology.erase(second);
    net.snapshot.net.transitions.pop_back();
    net.snapshot.transitionStates.erase(second);
    for (std::int64_t choice : {0, 1})
        CHECK(Plan(net, {first}, {choice}).selectedInNetOrder.empty());
}
TEST_CASE("Selection order is net order and conflict visitation order is explicit", "[conflict-selection]")
{
    Net net;
    std::reverse(net.snapshot.net.transitions.begin(), net.snapshot.net.transitions.end());
    CHECK(Plan(net, {}, {}).selectedInNetOrder == std::vector<TransitionId>{second, first});
    const auto plan = Plan(net, {second, first}, {0});
    CHECK(plan.decisions.front().visitedTransition == second);
    CHECK(plan.selectedInNetOrder == std::vector<TransitionId>{first});
}
TEST_CASE("Selection forwards topology and eligibility diagnostics", "[conflict-selection]")
{
    Net net;
    SECTION("Topology")
    {
        net.topology.at(place).outgoing.clear();
        CHECK(HasError(net, {}, {}, ValidationCode::MissingArcReference));
    }
    SECTION("Legacy range")
    {
        net.snapshot.net.places.front().capacity = 4294967295ULL;
        net.snapshot.marking.at(place).tokens = 4294967295ULL;
        CHECK(HasError(net, {}, {}, ValidationCode::UnsupportedLegacyRange));
    }
    SECTION("Invalid snapshot")
    {
        net.snapshot.marking.clear();
        CHECK(HasError(net, {}, {}, ValidationCode::MissingState));
    }
}
TEST_CASE("A losing transition continues resolving its other input conflicts", "[conflict-selection]")
{
    Net net;
    net.Transition(third);
    net.topology.at(place).outgoing = {ArcId{11}, ArcId{10}};
    net.Arc(other, second);
    net.Arc(other, third);
    net.Arc(other, first);
    const auto plan = Plan(net, {first, second, third}, {0, 1});
    CHECK(plan.selectedInNetOrder == std::vector<TransitionId>{third});
    REQUIRE(plan.decisions.size() == 2);
    CHECK(plan.decisions.back().visitedTransition == first);
    CHECK(plan.decisions.back().place == other);
}
TEST_CASE("Selection uses endpoint order independently of document arc order", "[conflict-selection]")
{
    Net net;
    std::reverse(net.topology.at(place).outgoing.begin(), net.topology.at(place).outgoing.end());
    CHECK(Plan(net, {first, second}, {0}).selectedInNetOrder == std::vector<TransitionId>{second});
    CHECK(net.snapshot.net.arcs.front().id == ArcId{10});
}
