#include "domain/ConflictTopology.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Topology headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

using namespace hps::domain;
namespace
{
constexpr PlaceId place{1};
constexpr TransitionId transition{1}; // Typed IDs may overlap in the pure model.
constexpr ArcId input{3}, output{4};
NetSnapshot Net()
{
    NetSnapshot net;
    net.net.places = {{place, u"P", 10, 0}};
    net.net.transitions = {{transition, u"T"}};
    net.net.arcs = {{input, u"In", place, transition}, {output, u"Out", transition, place}};
    net.marking = {{place, {1, 0}}};
    net.transitionStates = {{transition, {}}};
    return net;
}
ConflictTopology Topology()
{
    return {{place, {{output}, {input}}}, {transition, {{input}, {output}}}};
}
bool HasError(const NetSnapshot& net, const ConflictTopology& topology, ValidationCode code)
{
    const auto issues = ValidateConflictTopology(net, topology);
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) {
        return issue.code == code && !issue.message.empty();
    });
}
}

TEST_CASE("Topology validates directed self-loop with typed overlapping IDs", "[topology]")
{
    CHECK(ValidateConflictTopology(Net(), Topology()).empty());
    CHECK(ValidateConflictTopology({}, {}).empty());
}
TEST_CASE("Isolated nodes require explicit empty adjacency", "[topology]")
{
    auto net = Net();
    net.net.arcs.clear();
    CHECK(HasError(net, {}, ValidationCode::MissingAdjacency));
    CHECK(ValidateConflictTopology(net, {{place, {}}, {transition, {}}}).empty());
}
TEST_CASE("Topology rejects unknown and incorrectly typed node keys", "[topology]")
{
    auto topology = Topology();
    topology.emplace(PlaceId{99}, NodeAdjacency{});
    CHECK(HasError(Net(), topology, ValidationCode::UnexpectedAdjacency));
    topology = Topology();
    topology.erase(transition);
    CHECK(HasError(Net(), topology, ValidationCode::MissingAdjacency));
}
TEST_CASE("Topology rejects missing arc membership at either endpoint", "[topology]")
{
    auto topology = Topology();
    SECTION("Source") { topology.at(place).outgoing.clear(); }
    SECTION("Destination") { topology.at(transition).incoming.clear(); }
    CHECK(HasError(Net(), topology, ValidationCode::MissingArcReference));
}
TEST_CASE("Topology rejects unknown arc references", "[topology]")
{
    auto topology = Topology();
    topology.at(place).outgoing.push_back(ArcId{99});
    CHECK(HasError(Net(), topology, ValidationCode::InvalidArcReference));
}
TEST_CASE("Topology rejects duplicates in either direction", "[topology]")
{
    auto topology = Topology();
    SECTION("Outgoing") { topology.at(place).outgoing.push_back(input); }
    SECTION("Incoming") { topology.at(transition).incoming.push_back(input); }
    CHECK(HasError(Net(), topology, ValidationCode::DuplicateArcReference));
}
TEST_CASE("Topology rejects wrong direction and foreign endpoint membership", "[topology]")
{
    auto topology = Topology();
    SECTION("Direction") { topology.at(place).incoming.push_back(input); }
    SECTION("Endpoint") { topology.at(transition).outgoing.push_back(input); }
    CHECK(HasError(Net(), topology, ValidationCode::InvalidArcReference));
}
TEST_CASE("Topology preserves arbitrary order and distinct parallel arcs", "[topology]")
{
    auto net = Net();
    net.net.arcs.push_back({ArcId{5}, u"Parallel", place, transition, ArcKind::Test});
    auto topology = Topology();
    topology.at(place).outgoing = {ArcId{5}, input};
    topology.at(transition).incoming.push_back(ArcId{5});
    CHECK(ValidateConflictTopology(net, topology).empty());
    CHECK(topology.at(place).outgoing == std::vector<ArcId>{ArcId{5}, input});
    CHECK(topology.at(transition).incoming == std::vector<ArcId>{input, ArcId{5}});
    CHECK(net.net.arcs.front().id == input);
}
TEST_CASE("Topology validation forwards invalid snapshot diagnostics", "[topology]")
{
    auto net = Net();
    SECTION("Duplicate ID") { net.net.arcs.push_back(net.net.arcs.front()); CHECK(HasError(net, Topology(), ValidationCode::DuplicateId)); }
    SECTION("Endpoint") { net.net.arcs.front().from = PlaceId{99}; CHECK(HasError(net, Topology(), ValidationCode::MissingEndpoint)); }
    SECTION("Marking") { net.marking.clear(); CHECK(HasError(net, Topology(), ValidationCode::MissingState)); }
}
