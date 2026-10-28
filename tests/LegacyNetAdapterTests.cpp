#include "stdafx.h"
#include "PetriSimDoc.h"
#include "resource.h"
#include "HRect.h"
#include "adapters/LegacyNetAdapter.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <memory>

using namespace hps::domain;
using hps::adapters::ExportLegacyNet;

namespace
{
struct LegacyGraph
{
    CPetriSimDoc document;
    std::vector<std::unique_ptr<CHDrawObject>> objects;
    ULONG32 nextId = 0;
    template<class T> T& Add()
    {
        auto object = std::make_unique<T>(CPoint(0, 0), ++nextId);
        object->Id(nextId);
        T& result = *object;
        objects.push_back(std::move(object));
        document.DrawObjectList().AddTail(&result);
        return result;
    }
    CHConnector& Arc(CHNetMember& from, CHNetMember& to)
    {
        auto& arc = Add<CHConnector>();
        arc.From(&from);
        arc.To(&to);
        return arc;
    }
    ~LegacyGraph()
    {
        for (const auto& object : objects)
            if (auto* arc = dynamic_cast<CHConnector*>(object.get()))
            {
                arc->From(nullptr);
                arc->To(nullptr);
            }
        document.DrawObjectList().RemoveAll();
    }
};

bool HasError(CPetriSimDoc& document, ValidationCode code)
{
    const auto result = ExportLegacyNet(document);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    const auto& issues = std::get<std::vector<ValidationIssue>>(result);
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) {
        return issue.code == code && !issue.message.empty();
    });
}
}

TEST_CASE("Adapter copies names marking timing and statistics without mutation", "[adapter]")
{
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>();
    place.Name(L"Miesto \u017d \U0001F680");
    place.SetTokenMax(10);
    CHPropertyValues values;
    values.Add(CHPropertyValue(5L, ID_POSITION_TOKEN_INITIAL));
    REQUIRE(place.SetProperties(values, false));
    place.SetToken(3);
    place.SetTokenCount(9);
    auto& transition = graph.Add<CHTransition>();
    transition.TimeMode(CHTransition::TTM_EQUAL_DISTR);
    transition.SetStartDelay(2);
    transition.SetRangeDelay(7);
    transition.SetDelay(static_cast<UINT>(-1));
    transition.SetTokensCount(6);
    transition.Enabled(true);
    auto& arc = graph.Arc(place, transition);
    arc.Weigth(2);

    auto result = ExportLegacyNet(graph.document);
    REQUIRE(std::holds_alternative<NetSnapshot>(result));
    const auto& snapshot = std::get<NetSnapshot>(result);
    REQUIRE(snapshot.net.places.size() == 1);
    CHECK(snapshot.net.places[0].name == u"Miesto \u017d \U0001F680");
    CHECK(snapshot.net.places[0].initialTokens == 5);
    CHECK(snapshot.net.places[0].capacity == 10);
    CHECK(snapshot.marking.at(PlaceId{place.Id()}).tokens == 3);
    CHECK(snapshot.marking.at(PlaceId{place.Id()}).producedTokens == 9);
    CHECK(snapshot.net.transitions[0].mode == TimeMode::Uniform);
    CHECK(snapshot.net.transitions[0].initialDelayMs == 2);
    CHECK(snapshot.net.transitions[0].rangeMaxDelayMs == 7);
    const auto& state = snapshot.transitionStates.at(TransitionId{transition.Id()});
    CHECK(state.remainingDelayMs == -1);
    CHECK(state.consumedTokens == 6);
    CHECK(state.enabled);
    CHECK(snapshot.net.arcs[0].weight == 2);
    CHECK(std::get<PlaceId>(snapshot.net.arcs[0].from).value == place.Id());
    CHECK(std::get<TransitionId>(snapshot.net.arcs[0].to).value == transition.Id());
    CHECK(place.GetToken() == 3);
    CHECK(transition.GetDelay() == -1);
    CHECK(arc.From() == &place);

    place.Name(L"Changed");
    place.SetToken(0);
    transition.SetDelay(10);
    CHECK(snapshot.net.places[0].name == u"Miesto \u017d \U0001F680");
    CHECK(snapshot.marking.at(PlaceId{place.Id()}).tokens == 3);
    CHECK(state.remainingDelayMs == -1);
}

TEST_CASE("Adapter maps every arc kind and preserves document order", "[adapter]")
{
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>();
    auto& transition = graph.Add<CHTransition>();
    auto& normal = graph.Arc(place, transition);
    auto& inhibitor = graph.Arc(transition, place);
    inhibitor.Type(CHConnector::TypeInhibitor);
    auto& test = graph.Arc(place, transition);
    test.Type(CHConnector::TypeTest);
    auto result = ExportLegacyNet(graph.document);
    REQUIRE(std::holds_alternative<NetSnapshot>(result));
    const auto& arcs = std::get<NetSnapshot>(result).net.arcs;
    REQUIRE(arcs.size() == 3);
    CHECK(arcs[0].id.value == normal.Id());
    CHECK(arcs[0].kind == ArcKind::Normal);
    CHECK(arcs[1].id.value == inhibitor.Id());
    CHECK(arcs[1].kind == ArcKind::Inhibitor);
    CHECK(arcs[2].id.value == test.Id());
    CHECK(arcs[2].kind == ArcKind::Test);
}

TEST_CASE("Adapter maps every legacy time mode explicitly", "[adapter]")
{
    const std::array<TimeMode, 4> expected = {TimeMode::Immediate, TimeMode::FixedDelay, TimeMode::Exponential, TimeMode::Uniform};
    for (int mode = 0; mode < 4; ++mode)
    {
        LegacyGraph graph;
        auto& transition = graph.Add<CHTransition>();
        transition.TimeMode(static_cast<CHTransition::TransitionTimeMode>(mode));
        transition.SetStartDelay(5);
        transition.SetRangeDelay(10);
        auto result = ExportLegacyNet(graph.document);
        REQUIRE(std::holds_alternative<NetSnapshot>(result));
        CHECK(std::get<NetSnapshot>(result).net.transitions[0].mode == expected[mode]);
    }
}

TEST_CASE("Adapter preserves unsigned legacy count bits", "[adapter]")
{
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>();
    place.SetTokenMax(-1);
    place.SetToken(-2);
    place.SetTokenCount(-3);
    auto result = ExportLegacyNet(graph.document);
    REQUIRE(std::holds_alternative<NetSnapshot>(result));
    const auto& snapshot = std::get<NetSnapshot>(result);
    CHECK(snapshot.net.places[0].capacity == 4294967295ULL);
    CHECK(snapshot.marking.at(PlaceId{place.Id()}).tokens == 4294967294ULL);
    CHECK(snapshot.marking.at(PlaceId{place.Id()}).producedTokens == 4294967293ULL);
}

TEST_CASE("Adapter explicitly rejects disconnected and foreign endpoints", "[adapter]")
{
    SECTION("Disconnected")
    {
        LegacyGraph graph;
        graph.Add<CHConnector>();
        CHECK(HasError(graph.document, ValidationCode::DisconnectedArc));
    }
    SECTION("Foreign")
    {
        CHPosition foreign(CPoint(0, 0), 99);
        foreign.Id(99);
        LegacyGraph graph;
        auto& transition = graph.Add<CHTransition>();
        graph.Arc(foreign, transition);
        CHECK(HasError(graph.document, ValidationCode::ForeignEndpoint));
    }
}

TEST_CASE("Adapter diagnoses duplicate and stale legacy IDs", "[adapter]")
{
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>();
    auto& transition = graph.Add<CHTransition>();
    auto& arc = graph.Arc(place, transition);
    SECTION("Duplicate across node types")
    {
        transition.Id(place.Id());
        CHECK(HasError(graph.document, ValidationCode::DuplicateId));
    }
    SECTION("Stale reference")
    {
        arc.FromId(99);
        CHECK(HasError(graph.document, ValidationCode::StaleReference));
    }
}

TEST_CASE("Adapter reports invalid enums and domain invariants", "[adapter]")
{
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>();
    auto& transition = graph.Add<CHTransition>();
    auto& arc = graph.Arc(place, transition);
    SECTION("Time mode") { transition.TimeMode(static_cast<CHTransition::TransitionTimeMode>(99)); CHECK(HasError(graph.document, ValidationCode::InvalidEnum)); }
    SECTION("Arc kind") { arc.Type(static_cast<CHConnector::ConnectorType>(99)); CHECK(HasError(graph.document, ValidationCode::InvalidEnum)); }
    SECTION("Weight") { arc.Weigth(0); CHECK(HasError(graph.document, ValidationCode::InvalidWeight)); }
    SECTION("Marking") { place.SetToken(2); CHECK(HasError(graph.document, ValidationCode::CapacityExceeded)); }
}

TEST_CASE("Drawing-only documents project to an empty domain net", "[adapter]")
{
    LegacyGraph graph;
    auto rectangle = std::make_unique<CHRect>(CPoint(0, 0));
    graph.document.DrawObjectList().AddTail(rectangle.get());
    graph.objects.push_back(std::move(rectangle));
    auto result = ExportLegacyNet(graph.document);
    REQUIRE(std::holds_alternative<NetSnapshot>(result));
    const auto& net = std::get<NetSnapshot>(result).net;
    CHECK(net.places.empty());
    CHECK(net.transitions.empty());
    CHECK(net.arcs.empty());
}

TEST_CASE("Sample archives can be inspected through the domain adapter", "[adapter][samples]")
{
    class LoadedDocument : public CPetriSimDoc
    {
    public:
        ~LoadedDocument() override { DeleteContents(); }
    };
    for (const auto* name : {L"TokenGame", L"time_sample", L"demo_net", L"Philosophenproblem", L"prozess_connect", L"prozess_simple_connect"})
    {
        INFO(CStringA(name).GetString());
        LoadedDocument document;
        const CString path = CString(L"sample/") + name + L".hps";
        CFile file(path, CFile::modeRead | CFile::shareDenyWrite);
        CArchive archive(&file, CArchive::load);
        archive.m_pDocument = &document;
        document.Serialize(archive);
        auto result = ExportLegacyNet(document);
        std::string diagnostics;
        if (auto* issues = std::get_if<std::vector<ValidationIssue>>(&result))
            for (const auto& issue : *issues) diagnostics += std::to_string(issue.objectId) + ": " + issue.message + "\n";
        INFO(diagnostics);
        REQUIRE(std::holds_alternative<NetSnapshot>(result));
        const auto& net = std::get<NetSnapshot>(result).net;
        std::size_t places = 0, transitions = 0, arcs = 0;
        auto& objects = document.DrawObjectList();
        for (POSITION position = objects.GetHeadPosition(); position;)
        {
            auto* object = objects.GetNext(position);
            if (dynamic_cast<CHPosition*>(object)) ++places;
            else if (dynamic_cast<CHTransition*>(object)) ++transitions;
            else if (dynamic_cast<CHConnector*>(object)) ++arcs;
        }
        CHECK(net.places.size() == places);
        CHECK(net.transitions.size() == transitions);
        CHECK(net.arcs.size() == arcs);
        CHECK_FALSE(net.places.empty());
    }
}
