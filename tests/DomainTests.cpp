#include "domain/PetriNet.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Domain headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <type_traits>

using namespace hps::domain;
static_assert(!std::is_convertible_v<PlaceId, TransitionId>);
static_assert(!std::is_convertible_v<std::uint32_t, PlaceId>);

namespace
{
NetSnapshot ValidSnapshot()
{
    NetSnapshot snapshot;
    snapshot.net.places.push_back({PlaceId{1}, u"Miesto", 10, 2});
    snapshot.net.transitions.push_back({TransitionId{2}, u"Prechod", TimeMode::FixedDelay, 5, 0});
    snapshot.net.arcs.push_back({ArcId{3}, u"Hrana", PlaceId{1}, TransitionId{2}, ArcKind::Normal, 2});
    snapshot.marking.emplace(PlaceId{1}, PlaceState{4, 7});
    snapshot.transitionStates.emplace(TransitionId{2}, TransitionState{-1, 6, false, true});
    return snapshot;
}
bool Has(const NetSnapshot& snapshot, ValidationCode code)
{
    const auto issues = Validate(snapshot);
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return issue.code == code; });
}
}

TEST_CASE("A valid net preserves initial and current marking separately", "[domain]")
{
    const auto snapshot = ValidSnapshot();
    CHECK(Validate(snapshot).empty());
    CHECK(snapshot.net.places[0].initialTokens == 2);
    CHECK(snapshot.marking.at(PlaceId{1}).tokens == 4);
    CHECK(snapshot.transitionStates.at(TransitionId{2}).remainingDelayMs == -1);
}
TEST_CASE("Snapshots own independent names and marking", "[domain]")
{
    const auto original = ValidSnapshot();
    auto copy = original;
    copy.net.places[0].name = u"Zmena";
    copy.marking.at(PlaceId{1}).tokens = 9;
    CHECK(original.net.places[0].name == u"Miesto");
    CHECK(original.marking.at(PlaceId{1}).tokens == 4);
}
TEST_CASE("Zero and duplicate IDs are diagnosed", "[domain]")
{
    auto snapshot = ValidSnapshot();
    snapshot.net.places.push_back(snapshot.net.places[0]);
    CHECK(Has(snapshot, ValidationCode::DuplicateId));
    snapshot.net.places.back().id = PlaceId{};
    CHECK(Has(snapshot, ValidationCode::InvalidId));
}
TEST_CASE("Capacity and both markings are validated without repair", "[domain]")
{
    auto snapshot = ValidSnapshot();
    snapshot.net.places[0].capacity = 0;
    CHECK(Has(snapshot, ValidationCode::InvalidCapacity));
    snapshot.net.places[0].capacity = 3;
    CHECK(Has(snapshot, ValidationCode::CapacityExceeded));
    CHECK(snapshot.marking.at(PlaceId{1}).tokens == 4);
    snapshot.marking.at(PlaceId{1}).tokens = 1;
    snapshot.net.places[0].initialTokens = 4;
    CHECK(Has(snapshot, ValidationCode::CapacityExceeded));
}
TEST_CASE("Arc endpoints must exist and have opposite node types", "[domain]")
{
    auto snapshot = ValidSnapshot();
    snapshot.net.arcs[0].to = TransitionId{99};
    CHECK(Has(snapshot, ValidationCode::MissingEndpoint));
    snapshot.net.arcs[0].to = PlaceId{1};
    CHECK(Has(snapshot, ValidationCode::NonBipartiteArc));
}
TEST_CASE("Zero weights and unknown arc kinds are rejected", "[domain]")
{
    auto snapshot = ValidSnapshot();
    snapshot.net.arcs[0].weight = 0;
    CHECK(Has(snapshot, ValidationCode::InvalidWeight));
    snapshot.net.arcs[0].kind = static_cast<ArcKind>(99);
    CHECK(Has(snapshot, ValidationCode::InvalidEnum));
}
TEST_CASE("Every node needs exactly its own runtime state", "[domain]")
{
    auto snapshot = ValidSnapshot();
    snapshot.marking.clear();
    snapshot.transitionStates.clear();
    CHECK(Has(snapshot, ValidationCode::MissingState));
    snapshot.marking.emplace(PlaceId{99}, PlaceState{});
    snapshot.transitionStates.emplace(TransitionId{99}, TransitionState{});
    CHECK(Has(snapshot, ValidationCode::UnexpectedState));
}
TEST_CASE("Timing configuration is validated independently of remaining delay", "[domain]")
{
    auto snapshot = ValidSnapshot();
    auto& transition = snapshot.net.transitions[0];
    transition.initialDelayMs = -1;
    CHECK(Has(snapshot, ValidationCode::InvalidTiming));
    transition.initialDelayMs = 0;
    transition.mode = TimeMode::Exponential;
    CHECK(Has(snapshot, ValidationCode::InvalidTiming));
    transition.mode = TimeMode::Uniform;
    transition.initialDelayMs = 2;
    transition.rangeMaxDelayMs = 2;
    CHECK(Has(snapshot, ValidationCode::InvalidTiming));
    transition.rangeMaxDelayMs = 7;
    CHECK(Validate(snapshot).empty());
    transition.mode = static_cast<TimeMode>(99);
    CHECK(Has(snapshot, ValidationCode::InvalidEnum));
}
TEST_CASE("Typed ID spaces and empty nets are supported", "[domain]")
{
    CHECK(Validate(NetSnapshot{}).empty());
    auto snapshot = ValidSnapshot();
    snapshot.net.transitions[0].id = TransitionId{1};
    snapshot.net.arcs[0].to = TransitionId{1};
    snapshot.transitionStates.clear();
    snapshot.transitionStates.emplace(TransitionId{1}, TransitionState{});
    CHECK(Validate(snapshot).empty());
}
TEST_CASE("All arc kinds work in both directions without selecting firing semantics", "[domain]")
{
    auto snapshot = ValidSnapshot();
    for (auto kind : {ArcKind::Normal, ArcKind::Inhibitor, ArcKind::Test})
    {
        snapshot.net.arcs[0].kind = kind;
        CHECK(Validate(snapshot).empty());
        std::swap(snapshot.net.arcs[0].from, snapshot.net.arcs[0].to);
        CHECK(Validate(snapshot).empty());
    }
}
