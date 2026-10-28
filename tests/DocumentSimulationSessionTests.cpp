#include "application/DocumentSimulationSession.h"
#if defined(_MFC_VER) || defined(_WINDOWS_)
#error Session headers must not import MFC or Windows headers
#endif
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <stdexcept>

using namespace hps::application;
using namespace hps::simulation;
using namespace hps::domain;
using namespace std::chrono_literals;
namespace
{
constexpr PlaceId place{1};
constexpr TransitionId transition{2};
struct Net
{
    NetSnapshot snapshot;
    ConflictTopology topology;
    Net()
    {
        snapshot.net.places = {{place, u"P", 10, 8}};
        snapshot.marking = {{place, {8, 0}}};
        snapshot.net.transitions = {{transition, u"T", TimeMode::Uniform, 2, 7}};
        snapshot.transitionStates = {{transition, {}}};
        snapshot.net.arcs = {{ArcId{3}, u"In", place, transition}, {ArcId{4}, u"Out", transition, place}};
        topology = {{place, {{ArcId{4}}, {ArcId{3}}}}, {transition, {{ArcId{3}}, {ArcId{4}}}}};
    }
    std::unique_ptr<DocumentSimulationSession> Session(DocumentId id = {42}, const SessionOptions& options = {}) const
    {
        auto created = DocumentSimulationSession::Create(id, snapshot, topology, options);
        REQUIRE(std::holds_alternative<std::unique_ptr<DocumentSimulationSession>>(created));
        return std::get<std::unique_ptr<DocumentSimulationSession>>(std::move(created));
    }
};
CommandReply Command(DocumentSimulationSession& session, ServiceCommand command)
{
    auto pending = session.Submit(command);
    CHECK(pending.identity == session.Identity());
    REQUIRE(pending.completion.wait_for(5s) == std::future_status::ready);
    return pending.completion.get();
}
template<class Predicate> SessionSnapshot Until(DocumentSimulationSession& session, Predicate predicate)
{
    auto snapshot = session.Snapshot();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate(*snapshot.publication) && std::chrono::steady_clock::now() < deadline)
        snapshot = session.WaitForUpdate(snapshot.publication->revision, 100ms);
    REQUIRE(predicate(*snapshot.publication));
    return snapshot;
}
}

TEST_CASE("Session tags initial snapshots and command handles with typed identity", "[session]")
{
    auto session = Net{}.Session();
    const auto identity = session->Identity();
    CHECK(identity.document == DocumentId{42}); CHECK(identity.session.value != 0);
    CHECK(session->Accepts(identity)); CHECK_FALSE(session->IsClosed());
    CHECK_FALSE(session->Accepts({DocumentId{43}, identity.session}));
    const auto snapshot = session->Snapshot();
    CHECK(snapshot.identity == identity); CHECK(snapshot.publication->state == RunState::Paused);
    CHECK(Command(*session, ServiceCommand::Pause).outcome == CommandOutcome::Applied);
}
TEST_CASE("Replacing a session for one document rejects old snapshots and replies", "[session]")
{
    const Net net;
    auto old = net.Session();
    const auto retained = old->Snapshot();
    auto pending = old->Submit(ServiceCommand::Pause);
    old->Close();
    REQUIRE(pending.completion.wait_for(5s) == std::future_status::ready);
    pending.completion.get();
    auto replacement = net.Session();
    CHECK(replacement->Identity().document == retained.identity.document);
    CHECK(replacement->Identity().session != retained.identity.session);
    CHECK_FALSE(replacement->Accepts(retained.identity));
    CHECK_FALSE(replacement->Accepts(pending.identity));
    CHECK_FALSE(old->Accepts(retained.identity));
}
TEST_CASE("Close invalidates acceptance joins work and rejects new session commands", "[session]")
{
    auto session = Net{}.Session();
    const auto identity = session->Identity();
    auto pending = session->Submit(ServiceCommand::Resume);
    session->Close();
    CHECK(session->IsClosed()); CHECK_FALSE(session->Accepts(identity));
    CHECK(session->Snapshot().publication->closed); CHECK(session->Presentation().closed);
    CHECK(session->Completion().wait_for(0ms) == std::future_status::ready);
    session->Completion().get();
    REQUIRE(pending.completion.wait_for(0ms) == std::future_status::ready);
    pending.completion.get();
    CHECK_THROWS_AS(session->Submit(ServiceCommand::Resume), std::logic_error);
    session->Close();
}
TEST_CASE("Session copies input and presentation values cannot mutate the worker", "[session]")
{
    Net net;
    auto session = net.Session();
    net.snapshot.net.places.front().name = u"Changed";
    net.snapshot.marking.at(place).tokens = 0;
    net.topology.clear();
    auto view = session->Presentation();
    view.marking.at(place).tokens = 0; view.transitions.clear();
    const auto actual = session->Presentation();
    CHECK(actual.marking.at(place).tokens == 8); CHECK(actual.transitions.size() == 1);
    CHECK(session->Snapshot().publication->engine.net.net.places.front().name == u"P");
}
TEST_CASE("Presentation follows a seeded session without exposing legacy objects", "[session]")
{
    const Net net;
    SessionOptions options;
    options.clock.stepLimit = 25; options.batchSize = 3; options.seed = 99;
    auto session = net.Session({77}, options);
    Command(*session, ServiceCommand::Resume);
    const auto stopped = Until(*session, [](const auto& value) { return value.engine.stop.Locked(); });
    const auto view = session->Presentation();
    CHECK(view.identity == stopped.identity); CHECK(view.revision == stopped.publication->revision);
    CHECK(view.clock.steps == 26); CHECK(view.stop.stepLimitReached);
    CHECK(view.marking.at(place).tokens == 8); CHECK(view.state == RunState::Paused);
    auto created = SimulationEngine::Create(net.snapshot, net.topology, options.clock, options.seed);
    REQUIRE(std::holds_alternative<SimulationEngine>(created));
    RunController reference(std::get<SimulationEngine>(std::move(created)));
    REQUIRE(reference.Resume() == ResumeResult::Running);
    const auto expected = reference.RunBatch(100);
    CHECK(stopped.publication->engine.random == expected.snapshot.random);
    CHECK(view.transitions.at(transition).remainingDelayMs == expected.snapshot.net.transitionStates.at(transition).remainingDelayMs);
    CHECK(view.marking.at(place).producedTokens == expected.snapshot.net.marking.at(place).producedTokens);
}
TEST_CASE("Session public methods reject access from a different owner thread", "[session]")
{
    auto session = Net{}.Session();
    const auto identity = session->Identity();
    auto checked = std::async(std::launch::async, [&] {
        auto rejects = [](auto action) {
            try { action(); return false; }
            catch (const std::logic_error&) { return true; }
        };
        return rejects([&] { session->Identity(); }) && rejects([&] { session->Accepts(identity); }) &&
            rejects([&] { session->IsClosed(); }) && rejects([&] { session->Snapshot(); }) &&
            rejects([&] { session->Presentation(); }) && rejects([&] { session->Completion(); }) &&
            rejects([&] { session->Submit(ServiceCommand::Resume); }) &&
            rejects([&] { session->WaitForUpdate(0, 0ms); }) && rejects([&] { session->Close(); });
    });
    CHECK(checked.get());
    CHECK_FALSE(session->IsClosed()); CHECK(session->Snapshot().publication->engine.clock.steps == 0);
}
TEST_CASE("Session projects domain diagnostics until explicit reset", "[session]")
{
    Net net;
    net.snapshot.net.transitions.front().mode = TimeMode::Exponential;
    net.snapshot.net.transitions.front().initialDelayMs = 2147483647;
    SessionOptions options; options.seed = 10000;
    auto session = net.Session({42}, options);
    Command(*session, ServiceCommand::Resume);
    Until(*session, [](const auto& value) { return value.state == RunState::Failed; });
    const auto view = session->Presentation();
    REQUIRE_FALSE(view.issues.empty()); CHECK(view.issues.front().code == ValidationCode::ArithmeticOverflow);
    CHECK(view.clock.steps == 0); CHECK_FALSE(view.closed);
    CHECK(Command(*session, ServiceCommand::Resume).outcome == CommandOutcome::ResetRequired);
    Command(*session, ServiceCommand::ResetLegacy);
    CHECK(session->Presentation().issues.empty());
}
TEST_CASE("Session factory rejects invalid identity data and execution options", "[session]")
{
    Net net;
    DocumentId id{42}; SessionOptions options;
    auto expected = ValidationCode::InvalidId;
    SECTION("Identity") { id.value = 0; }
    SECTION("Topology") { net.topology.clear(); expected = ValidationCode::MissingAdjacency; }
    SECTION("Batch") { options.batchSize = 0; expected = ValidationCode::InvalidSimulationSettings; }
    SECTION("Queue") { options.queueCapacity = 0; expected = ValidationCode::InvalidSimulationSettings; }
    SECTION("Clock") { options.clock.sampleTimeMs = 0; expected = ValidationCode::InvalidSimulationSettings; }
    const auto result = DocumentSimulationSession::Create(id, net.snapshot, net.topology, options);
    REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(result));
    CHECK(std::get<std::vector<ValidationIssue>>(result).front().code == expected);
}
TEST_CASE("Session destruction joins and retained value handles outlive it", "[session]")
{
    std::shared_future<void> completion;
    SessionSnapshot retained;
    {
        auto session = Net{}.Session();
        retained = session->Snapshot(); completion = session->Completion();
        Command(*session, ServiceCommand::Resume);
        Until(*session, [](const auto& value) { return value.engine.clock.steps > 0; });
    }
    CHECK(completion.wait_for(0ms) == std::future_status::ready); completion.get();
    CHECK(retained.publication->engine.clock.steps == 0);
    CHECK(retained.publication->engine.net.marking.at(place).tokens == 8);
}
TEST_CASE("Session IDs remain unique across concurrent creators and full-width document IDs", "[session]")
{
    auto create = [] {
        std::vector<std::uint64_t> identities;
        const Net net;
        for (int index = 0; index < 10; ++index)
        {
            auto result = DocumentSimulationSession::Create(DocumentId{18446744073709551615ULL}, net.snapshot, net.topology);
            auto session = std::get<std::unique_ptr<DocumentSimulationSession>>(std::move(result));
            identities.push_back(session->Identity().session.value);
            session->Close();
        }
        return identities;
    };
    auto first = std::async(std::launch::async, create); auto second = std::async(std::launch::async, create);
    std::set<std::uint64_t> unique;
    for (auto* result : {&first, &second})
        for (auto id : result->get()) CHECK(unique.insert(id).second);
    CHECK(unique.size() == 20);
}
