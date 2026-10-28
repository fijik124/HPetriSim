#include "domain/ConflictScan.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Scan headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

using namespace hps::domain;
namespace
{
constexpr PlaceId input{1}, output{2};
constexpr TransitionId first{3}, second{4};
struct Net
{
    NetSnapshot snapshot;
    ConflictTopology topology;
    Net()
    {
        snapshot.net.places = {{input, u"P", 10, 0}, {output, u"Q", 10, 0}};
        snapshot.marking = {{input, {0, 0}}, {output, {10, 0}}};
        snapshot.net.transitions = {{first, u"T"}, {second, u"U"}};
        snapshot.transitionStates = {{first, {7, 5, true, true}}, {second, {}}};
        for (NodeId id : {NodeId{input}, NodeId{output}, NodeId{first}, NodeId{second}})
            topology.emplace(id, NodeAdjacency{});
    }
    void Arc(NodeId from, NodeId to, ArcKind kind = ArcKind::Normal)
    {
        const ArcId id{static_cast<std::uint32_t>(snapshot.net.arcs.size() + 10)};
        snapshot.net.arcs.push_back({id, u"A", from, to, kind, 1});
        topology.at(from).outgoing.push_back(id);
        topology.at(to).incoming.push_back(id);
    }
    std::vector<TransitionId> Scan() const
    {
        const auto result = FindLegacyPotentialConflicts(snapshot, topology);
        REQUIRE(std::holds_alternative<std::vector<TransitionId>>(result));
        return std::get<std::vector<TransitionId>>(result);
    }
};
}

TEST_CASE("Empty and isolated nets have no potential conflicts", "[conflict-scan]")
{
    CHECK(Net{}.Scan().empty());
    const auto empty = FindLegacyPotentialConflicts({}, {});
    REQUIRE(std::holds_alternative<std::vector<TransitionId>>(empty));
    CHECK(std::get<std::vector<TransitionId>>(empty).empty());
}
TEST_CASE("A chain with single endpoint peers has no potential conflicts", "[conflict-scan]")
{
    Net net;
    net.Arc(input, first);
    net.Arc(first, output);
    net.Arc(output, second);
    CHECK(net.Scan().empty());
}
TEST_CASE("Branch pre-scan includes every arc kind despite blocked or waiting state", "[conflict-scan]")
{
    for (auto kind : {ArcKind::Normal, ArcKind::Test, ArcKind::Inhibitor})
    {
        Net net;
        net.Arc(input, first, kind);
        net.Arc(input, second, kind);
        CHECK(net.Scan() == std::vector<TransitionId>{first, second});
        CHECK(net.snapshot.marking.at(input).tokens == 0);
        CHECK(net.snapshot.transitionStates.at(first).remainingDelayMs == 7);
        CHECK(net.snapshot.transitionStates.at(first).consumedTokens == 5);
        CHECK(net.snapshot.transitionStates.at(first).active);
        CHECK(net.snapshot.transitionStates.at(first).enabled);
    }
}
TEST_CASE("Meet pre-scan includes every arc kind at full capacity", "[conflict-scan]")
{
    for (auto kind : {ArcKind::Normal, ArcKind::Test, ArcKind::Inhibitor})
    {
        Net net;
        net.Arc(first, output, kind);
        net.Arc(second, output, kind);
        CHECK(net.Scan() == std::vector<TransitionId>{first, second});
    }
}
TEST_CASE("Distinct parallel arcs mark their sole transition once", "[conflict-scan]")
{
    Net net;
    SECTION("Input") { net.Arc(input, first); net.Arc(input, first); }
    SECTION("Output") { net.Arc(first, output); net.Arc(first, output); }
    CHECK(net.Scan() == std::vector<TransitionId>{first});
}
TEST_CASE("Multiple shared inputs and outputs do not duplicate transitions", "[conflict-scan]")
{
    Net net;
    net.Arc(input, first);
    net.Arc(input, second);
    net.Arc(first, output);
    net.Arc(second, output);
    net.Arc(output, first);
    net.Arc(output, second);
    CHECK(net.Scan() == std::vector<TransitionId>{first, second});
}
TEST_CASE("Potential conflict order follows transition order instead of arc or key order", "[conflict-scan]")
{
    Net net;
    net.Arc(input, first);
    net.Arc(input, second);
    std::reverse(net.snapshot.net.transitions.begin(), net.snapshot.net.transitions.end());
    CHECK(net.Scan() == std::vector<TransitionId>{second, first});
    std::reverse(net.topology.at(input).outgoing.begin(), net.topology.at(input).outgoing.end());
    CHECK(net.Scan() == std::vector<TransitionId>{second, first});
}
TEST_CASE("Pre-scan rejects invalid snapshots and incomplete topology without partial results", "[conflict-scan]")
{
    Net net;
    net.Arc(input, first);
    net.Arc(input, second);
    ValidationCode expected = ValidationCode::MissingArcReference;
    SECTION("Topology") { net.topology.at(first).incoming.clear(); }
    SECTION("Unknown arc") { net.topology.at(first).incoming.push_back(ArcId{99}); expected = ValidationCode::InvalidArcReference; }
    SECTION("Snapshot") { net.snapshot.marking.clear(); expected = ValidationCode::MissingState; }
    const auto result = FindLegacyPotentialConflicts(net.snapshot, net.topology);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    const auto& issues = std::get<std::vector<ValidationIssue>>(result);
    CHECK(std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == expected; }));
}
