#include "stdafx.h"
#include "PetriSimDoc.h"
#include "resource.h"
#include "HRect.h"
#include "adapters/LegacyNetAdapter.h"
#include "adapters/LegacySessionAdapter.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <memory>

using namespace hps::domain;
using hps::adapters::ExportLegacyNet;
using hps::adapters::ExportLegacySimulationSnapshot;
using hps::adapters::LegacySimulationSnapshot;

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

struct RestoreAdjacency
{
    Connectors& target;
    std::vector<CHConnector*> saved;
    explicit RestoreAdjacency(Connectors& connectors) : target(connectors)
    {
        for (INT_PTR i = 0; i < target.GetCount(); ++i) saved.push_back(target[i]);
    }
    ~RestoreAdjacency()
    {
        target.RemoveAll();
        for (auto* arc : saved) target.Add(arc);
    }
};

bool HasTopologyError(CPetriSimDoc& document, ValidationCode code)
{
    const auto result = ExportLegacySimulationSnapshot(document);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    const auto& issues = std::get<std::vector<ValidationIssue>>(result);
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) {
        return issue.code == code && !issue.message.empty();
    });
}

void CheckActualAdjacency(CPetriSimDoc& document)
{
    const auto result = ExportLegacySimulationSnapshot(document);
    std::string diagnostics;
    if (const auto* issues = std::get_if<std::vector<ValidationIssue>>(&result))
        for (const auto& issue : *issues) diagnostics += std::to_string(issue.objectId) + ": " + issue.message + "\n";
    INFO(diagnostics);
    REQUIRE(std::holds_alternative<LegacySimulationSnapshot>(result));
    const auto& projected = std::get<LegacySimulationSnapshot>(result);
    CHECK(ValidateConflictTopology(projected.snapshot, projected.topology).empty());
    std::size_t nodeCount = 0;
    auto& objects = document.DrawObjectList();
    for (POSITION position = objects.GetHeadPosition(); position;)
    {
        auto* object = objects.GetNext(position);
        auto* member = dynamic_cast<CHNetMember*>(object);
        if (!member) continue;
        ++nodeCount;
        NodeId node = dynamic_cast<CHPosition*>(member)
            ? NodeId{PlaceId{member->Id()}} : NodeId{TransitionId{member->Id()}};
        const auto& adjacency = projected.topology.at(node);
        auto check = [](Connectors& actual, const std::vector<ArcId>& copied) {
            REQUIRE(copied.size() == static_cast<std::size_t>(actual.GetCount()));
            for (INT_PTR index = 0; index < actual.GetCount(); ++index)
                CHECK(copied[static_cast<std::size_t>(index)].value == actual[index]->Id());
        };
        check(member->ConnectorsIn(), adjacency.incoming);
        check(member->ConnectorsOut(), adjacency.outgoing);
    }
    CHECK(projected.topology.size() == nodeCount);
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
        CheckActualAdjacency(document);
    }
}

TEST_CASE("Topology export preserves reconnection order independently of document order", "[adapter-topology]")
{
    for (bool branch : {true, false})
    {
        LegacyGraph graph;
        auto& place = graph.Add<CHPosition>();
        auto& first = graph.Add<CHTransition>();
        auto& second = graph.Add<CHTransition>();
        auto& firstArc = branch ? graph.Arc(place, first) : graph.Arc(first, place);
        auto& secondArc = branch ? graph.Arc(place, second) : graph.Arc(second, place);
        if (branch) { firstArc.From(nullptr); firstArc.From(&place); }
        else { firstArc.To(nullptr); firstArc.To(&place); }
        const auto result = ExportLegacySimulationSnapshot(graph.document);
        REQUIRE(std::holds_alternative<LegacySimulationSnapshot>(result));
        const auto& copy = std::get<LegacySimulationSnapshot>(result);
        const auto& adjacency = copy.topology.at(PlaceId{place.Id()});
        const auto& ordered = branch ? adjacency.outgoing : adjacency.incoming;
        CHECK(ordered == std::vector<ArcId>{ArcId{secondArc.Id()}, ArcId{firstArc.Id()}});
        CHECK(copy.snapshot.net.arcs.front().id.value == firstArc.Id());
        CheckActualAdjacency(graph.document);
        // Reconnect again, then change data: the returned value retains both copies.
        if (branch) { secondArc.From(nullptr); secondArc.From(&place); }
        else { secondArc.To(nullptr); secondArc.To(&place); }
        firstArc.Weigth(7);
        CHECK(ordered.front().value == secondArc.Id());
        CHECK(copy.snapshot.net.arcs.front().weight == 1);
    }
}

TEST_CASE("Topology export includes isolated nodes and distinct mixed parallel arcs", "[adapter-topology]")
{
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>();
    auto& transition = graph.Add<CHTransition>();
    auto& isolatedPlace = graph.Add<CHPosition>();
    auto& isolatedTransition = graph.Add<CHTransition>();
    graph.Arc(place, transition);
    graph.Arc(place, transition).Type(CHConnector::TypeTest);
    graph.Arc(transition, place).Type(CHConnector::TypeInhibitor);
    CheckActualAdjacency(graph.document);
    const auto result = ExportLegacySimulationSnapshot(graph.document);
    REQUIRE(std::holds_alternative<LegacySimulationSnapshot>(result));
    const auto& topology = std::get<LegacySimulationSnapshot>(result).topology;
    CHECK(topology.at(PlaceId{isolatedPlace.Id()}).incoming.empty());
    CHECK(topology.at(PlaceId{isolatedPlace.Id()}).outgoing.empty());
    CHECK(topology.at(TransitionId{isolatedTransition.Id()}).incoming.empty());
    CHECK(topology.at(TransitionId{isolatedTransition.Id()}).outgoing.empty());
}

TEST_CASE("Topology export diagnoses corrupted registered adjacency", "[adapter-topology]")
{
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>();
    auto& transition = graph.Add<CHTransition>();
    auto& arc = graph.Arc(place, transition);
    RestoreAdjacency restore(place.ConnectorsOut());
    RestoreAdjacency restoreIncoming(place.ConnectorsIn());
    SECTION("Missing")
    {
        place.ConnectorsOut().RemoveAll();
        CHECK(HasTopologyError(graph.document, ValidationCode::MissingArcReference));
    }
    SECTION("Duplicate")
    {
        place.ConnectorsOut().Add(&arc);
        CHECK(HasTopologyError(graph.document, ValidationCode::DuplicateArcReference));
    }
    SECTION("Wrong direction")
    {
        place.ConnectorsIn().Add(&arc);
        CHECK(HasTopologyError(graph.document, ValidationCode::InvalidArcReference));
    }
    SECTION("Wrong node")
    {
        RestoreAdjacency restoreOther(transition.ConnectorsOut());
        transition.ConnectorsOut().Add(&arc);
        CHECK(HasTopologyError(graph.document, ValidationCode::InvalidArcReference));
    }
}

TEST_CASE("Topology export rejects null and unregistered adjacency pointers", "[adapter-topology]")
{
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>();
    CHConnector foreign(CPoint(0, 0), 99);
    RestoreAdjacency restore(place.ConnectorsOut());
    SECTION("Null") { place.ConnectorsOut().Add(nullptr); }
    SECTION("Unregistered") { place.ConnectorsOut().Add(&foreign); }
    CHECK(HasTopologyError(graph.document, ValidationCode::StaleReference));
}

TEST_CASE("Topology export forwards net validation failures", "[adapter-topology]")
{
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>();
    auto& transition = graph.Add<CHTransition>();
    auto& arc = graph.Arc(place, transition);
    SECTION("Disconnected") { arc.To(nullptr); CHECK(HasTopologyError(graph.document, ValidationCode::DisconnectedArc)); }
    SECTION("Stale endpoint ID") { arc.FromId(99); CHECK(HasTopologyError(graph.document, ValidationCode::StaleReference)); }
    SECTION("Duplicate ID") { transition.Id(place.Id()); CHECK(HasTopologyError(graph.document, ValidationCode::DuplicateId)); }
}

TEST_CASE("Empty and drawing-only documents export empty topology", "[adapter-topology]")
{
    LegacyGraph graph;
    SECTION("Empty") {}
    SECTION("Drawing only")
    {
        auto rectangle = std::make_unique<CHRect>(CPoint(0, 0));
        graph.document.DrawObjectList().AddTail(rectangle.get());
        graph.objects.push_back(std::move(rectangle));
    }
    const auto result = ExportLegacySimulationSnapshot(graph.document);
    REQUIRE(std::holds_alternative<LegacySimulationSnapshot>(result));
    CHECK(std::get<LegacySimulationSnapshot>(result).topology.empty());
}

TEST_CASE("Legacy session copies values without writing back source edits", "[session-adapter]")
{
    using namespace hps::application;
    using namespace hps::simulation;
    LegacyGraph graph;
    auto& place = graph.Add<CHPosition>(); place.SetToken(1);
    auto& transition = graph.Add<CHTransition>();
    auto& arc = graph.Arc(place, transition);
    auto created = hps::adapters::CreateLegacyDocumentSession(graph.document, DocumentId{51}, 99);
    REQUIRE(std::holds_alternative<std::unique_ptr<DocumentSimulationSession>>(created));
    auto session = std::get<std::unique_ptr<DocumentSimulationSession>>(std::move(created));
    place.SetToken(0); place.SetTokenCount(55); place.Name(L"Edited"); arc.To(nullptr);
    CHECK(session->Presentation().marking.at(PlaceId{place.Id()}).tokens == 1);
    CHECK(session->Snapshot().publication->engine.random.value == 99);
    auto pending = session->Submit(ServiceCommand::ResetLegacy);
    REQUIRE(pending.completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(pending.completion.get().outcome == CommandOutcome::Applied);
    CHECK(place.GetTokenCount() == 55); CHECK(place.Name() == L"Edited");
    CHECK_FALSE(arc.Connected()); CHECK_FALSE(graph.document.SimOnline());
    session->Close();
}

TEST_CASE("Legacy session remains value-owned after source document and objects are destroyed", "[session-adapter]")
{
    using namespace hps::application;
    using namespace hps::simulation;
    std::unique_ptr<DocumentSimulationSession> session;
    {
        LegacyGraph graph;
        auto& place = graph.Add<CHPosition>(); place.SetToken(1);
        auto& transition = graph.Add<CHTransition>(); graph.Arc(place, transition);
        auto created = hps::adapters::CreateLegacyDocumentSession(graph.document, DocumentId{52});
        REQUIRE(std::holds_alternative<std::unique_ptr<DocumentSimulationSession>>(created));
        session = std::get<std::unique_ptr<DocumentSimulationSession>>(std::move(created));
    }
    auto pending = session->Submit(ServiceCommand::Resume);
    REQUIRE(pending.completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(pending.completion.get().outcome == CommandOutcome::Applied);
    auto snapshot = session->Snapshot();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!snapshot.publication->engine.stop.Locked() && std::chrono::steady_clock::now() < deadline)
        snapshot = session->WaitForUpdate(snapshot.publication->revision, std::chrono::milliseconds(100));
    CHECK(snapshot.publication->engine.stop.deadlock);
    CHECK(snapshot.publication->engine.clock.steps == 2);
    session->Close();
}

TEST_CASE("Legacy session reports export identity and clock errors explicitly", "[session-adapter]")
{
    using namespace hps::application;
    LegacyGraph graph;
    graph.Add<CHPosition>();
    DocumentId id{53};
    auto code = ValidationCode::DisconnectedArc;
    SECTION("Disconnected") { graph.Add<CHConnector>(); }
    SECTION("Clock") { graph.document.Sim().SampleTime(0); code = ValidationCode::InvalidSimulationSettings; }
    SECTION("Identity") { id.value = 0; code = ValidationCode::InvalidId; }
    const auto result = hps::adapters::CreateLegacyDocumentSession(graph.document, id);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    CHECK(std::get<std::vector<ValidationIssue>>(result).front().code == code);
}

TEST_CASE("All sample archives create and close offline sessions without conversion", "[session-adapter][samples]")
{
    using namespace hps::application;
    class LoadedDocument : public CPetriSimDoc
    {
    public:
        ~LoadedDocument() override { DeleteContents(); }
    };
    for (const auto* name : {L"TokenGame", L"time_sample", L"demo_net", L"Philosophenproblem", L"prozess_connect", L"prozess_simple_connect"})
    {
        INFO(CStringA(name).GetString());
        LoadedDocument document;
        CFile file(CString(L"sample/") + name + L".hps", CFile::modeRead | CFile::shareDenyWrite);
        CArchive archive(&file, CArchive::load); archive.m_pDocument = &document;
        document.Serialize(archive);
        auto created = hps::adapters::CreateLegacyDocumentSession(document, DocumentId{54});
        std::string diagnostics;
        if (const auto* issues = std::get_if<std::vector<ValidationIssue>>(&created))
            for (const auto& issue : *issues) diagnostics += issue.message + "\n";
        INFO(diagnostics);
        REQUIRE(std::holds_alternative<std::unique_ptr<DocumentSimulationSession>>(created));
        auto session = std::get<std::unique_ptr<DocumentSimulationSession>>(std::move(created));
        CHECK(session->Presentation().state == hps::simulation::RunState::Paused);
        CHECK(session->Presentation().clock.timeMs == document.Sim().SimTime());
        CHECK(session->Presentation().clock.steps == document.Sim().StepCount());
        CHECK(session->Presentation().clock.timeLimitMs == document.Sim().TimeLimit());
        CHECK(session->Presentation().clock.stepLimit == document.Sim().StepLimit());
        CHECK_FALSE(document.SimOnline());
        session->Close();
        CHECK(session->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    }
}
